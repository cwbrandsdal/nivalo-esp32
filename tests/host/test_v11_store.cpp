// Durable protocol state through the real NivaloDeviceStore (on a map-backed Preferences): what a
// credential change may do to the journal and the boot counter (section 15), and how unreadable
// records read (OTA floor, catalog). The executor is booted from the store as the device does.

#include <Preferences.h>

#include "NivaloArguments.h"
#include "NivaloDeviceStore.h"
#include "NivaloDownlink.h"
#include "NivaloExecutor.h"
#include "v11_test_support.h"

#include <cstring>
#include <vector>

using namespace v11test;

namespace
{
const char DeviceId[] = "22222222-2222-4222-8222-222222222222";
const char OtherDeviceId[] = "77777777-7777-4777-8777-777777777777";
const char ProductId[] = "11111111-1111-4111-8111-111111111111";

FakeNvs &nvs() { return FakeNvs::instance(); }

// The executor's platform on the real store; everything else is recorded.
class StorePlatform : public NivaloExecutorPlatform
{
public:
    explicit StorePlatform(NivaloDeviceStore &store) : _store(store) {}
    std::vector<NivaloAck> acks;
    int handlerCalls = 0;

    bool writeJournal(const NivaloJournal &journal) override { return _store.writeJournal(journal); }
    bool writePending(const NivaloPendingRecord &pending) override { return _store.writePending(pending); }
    bool clearPending() override { return _store.clearPending(); }
    bool writeFloor(uint32_t floor) override { return _store.writeFloor(ProductId, floor); }
    NivaloClockReading readClock() override
    {
        NivaloClockReading reading;
        reading.valid = true;
        nivaloParseUtc("2026-11-24T12:00:30Z", &reading.utcMs);
        reading.monotonicUs = 5000000000LL;
        return reading;
    }
    void publishAck(const NivaloAck &ack) override { acks.push_back(ack); }
    bool hasFunction(const char *) override { return true; }
    bool prepareFunction(const NivaloOperation &operation) override
    {
        return _arguments.prepare(operation.arguments, operation.argumentsLength);
    }
    NivaloFunctionOutcome invokePrepared(uint32_t) override
    {
        handlerCalls++;
        return NIVALO_FUNCTION_SUCCEEDED;
    }
    void releasePrepared() override { _arguments.release(); }
    NivaloRefreshOutcome refreshCatalog() override { return NIVALO_REFRESH_COMPLETE; }
    NivaloDownloadOutcome downloadImage(const NivaloOperation &, uint32_t) override { return NIVALO_DOWNLOAD_REJECTED; }
    NivaloActivationOutcome activateImage() override { return NIVALO_ACTIVATION_REJECTED; }
    bool hashRunningSlot(uint32_t, char *) override { return false; }
    void restart() override {}

private:
    NivaloDeviceStore &_store;
    NivaloPreparedArguments _arguments;
};

NivaloOperation command(const char *deviceId, uint32_t seq)
{
    char id[37];
    std::snprintf(id, sizeof(id), "44444444-4444-4444-8444-4444%08lx", (unsigned long)seq);
    const std::string payload = "{\"commandSeq\":" + std::to_string(seq) +
                                ",\"issuedAt\":\"2026-11-24T12:00:00Z\",\"expiresAt\":\"2026-11-24T12:05:00Z\","
                                "\"command\":\"setLed\",\"arguments\":{\"state\":true}}";
    const std::string envelope = std::string("{\"schema\":\"nivalo.iot.v1.1\",\"messageId\":\"33333333-3333-4333-8333-3333000000ee\",") +
                                 "\"deviceId\":\"" + deviceId + "\",\"sentAt\":\"2026-11-24T12:00:05Z\",\"correlationId\":\"" + id +
                                 "\",\"payload\":" + payload + "}";
    const std::string topic = std::string("nivalo/v1.1/devices/") + deviceId + "/commands/" + id;
    NivaloTopics topics;
    topics.configure(deviceId);
    NivaloOperation operation;
    V11_CHECK(nivaloParseDownlink(topics, topic.c_str(), reinterpret_cast<const uint8_t *>(envelope.data()),
                                  envelope.size(), &operation) == NIVALO_DOWNLINK_IDENTIFIED &&
                  operation.valid,
              "command did not parse");
    return operation;
}

// A boot as NivaloDevice::beginMqtt performs it: the stored journal (NULL when unusable) into a
// fresh executor. Returns whether the boot counter is readable (the device starts no session
// otherwise).
bool boot(NivaloDeviceStore &store, StorePlatform &platform, NivaloExecutor &executor, const char *deviceId)
{
    store.end();
    store.begin();
    NivaloJournal journal;
    const NivaloStoreRead read = store.loadJournal(&journal);
    uint32_t counter = 0U;
    const bool bootReadable = store.loadBoot(&counter) == NIVALO_STORE_OK;
    static NivaloOtaDevice ota;
    static NivaloImageIdentity image;
    executor.begin(deviceId, read == NIVALO_STORE_OK ? &journal : NULL, NULL, &ota, image);
    platform.acks.clear();
    platform.handlerCalls = 0;
    return bootReadable;
}

size_t protocolWrites()
{
    size_t count = 0U;
    for (const std::string &key : nvs().writes)
    {
        count += key.compare(0, 11, "nivalo-dev/") == 0 ? 1U : 0U;
    }
    return count;
}

int identityCases()
{
    int checked = 0;
    nvs().reset();
    NivaloDeviceStore store;
    StorePlatform platform(store);

    context() = "first enrollment initializes the protocol state";
    V11_CHECK(store.admitIdentity(DeviceId, false) == NIVALO_IDENTITY_FIRST_ENROLLMENT, "admission");
    V11_CHECK(store.initializeIdentity(DeviceId) && store.identityReady(DeviceId), "initialized");
    checked++;

    context() = "an executed operation whose terminal ACK was lost";
    {
        NivaloExecutor executor(platform);
        V11_CHECK(boot(store, platform, executor, DeviceId), "boot counter readable");
        executor.deliver(command(DeviceId, 41U));
        V11_CHECK(platform.handlerCalls == 1 && executor.journal().highWater == 41U, "operation 41 executed");
        // The ACKs never reach the server; the operation stays live there.
    }
    uint32_t counter = 0U;
    V11_CHECK(store.writeBoot(5U) && store.loadBoot(&counter) == NIVALO_STORE_OK && counter == 5U, "boot counter");
    checked++;

    context() = "a same-ID credential update keeps the high-water mark and the boot counter";
    {
        const size_t before = protocolWrites();
        V11_CHECK(store.admitIdentity(DeviceId, true) == NIVALO_IDENTITY_KEEP, "same device ID is kept");
        V11_CHECK(protocolWrites() == before, "the admission wrote protocol state");
        NivaloExecutor executor(platform);
        V11_CHECK(boot(store, platform, executor, DeviceId), "boot counter readable");
        executor.deliver(command(DeviceId, 41U));
        V11_CHECK(platform.handlerCalls == 0, "a redelivery of 41 ran the handler again");
        V11_CHECK(platform.acks.size() == 1U && platform.acks[0].result.status == NIVALO_ACK_SUCCEEDED,
                  "the redelivery is answered from the journal");
        V11_CHECK(store.loadBoot(&counter) == NIVALO_STORE_OK && counter == 5U, "boot counter reseeded");
    }
    checked++;

    context() = "a missing journal of an ID that has credentials blocks instead of reseeding";
    {
        const FakeNvs::Entry journal = nvs().entries["nivalo-dev/journal"];
        nvs().entries.erase("nivalo-dev/journal");
        const size_t before = protocolWrites();
        V11_CHECK(store.admitIdentity(DeviceId, true) == NIVALO_IDENTITY_REFUSED, "credentials admitted");
        V11_CHECK(protocolWrites() == before && nvs().entries.count("nivalo-dev/journal") == 0U, "journal reseeded");
        NivaloExecutor executor(platform);
        boot(store, platform, executor, DeviceId);
        executor.deliver(command(DeviceId, 41U));
        V11_CHECK(executor.blocked() && platform.handlerCalls == 0 && platform.acks.empty(),
                  "a device without its journal executed or answered");
        nvs().entries["nivalo-dev/journal"] = journal;
    }
    checked++;

    context() = "an unreadable journal or boot counter is not reseeded either";
    {
        nvs().unreadable.insert("nivalo-dev/journal");
        V11_CHECK(store.admitIdentity(DeviceId, true) == NIVALO_IDENTITY_REFUSED, "unreadable journal admitted");
        nvs().unreadable.clear();
        const FakeNvs::Entry bootEntry = nvs().entries["nivalo-dev/boot"];
        nvs().entries.erase("nivalo-dev/boot");
        const size_t before = protocolWrites();
        V11_CHECK(store.admitIdentity(DeviceId, true) == NIVALO_IDENTITY_REFUSED, "missing boot counter admitted");
        V11_CHECK(protocolWrites() == before && nvs().entries.count("nivalo-dev/boot") == 0U, "boot counter reseeded");
        nvs().entries["nivalo-dev/boot"] = bootEntry;
    }
    checked++;

    context() = "manual credentials of another device ID never take over the history";
    V11_CHECK(store.admitIdentity(OtherDeviceId, true) == NIVALO_IDENTITY_REFUSED, "switch with credentials");
    V11_CHECK(store.admitIdentity(OtherDeviceId, false) == NIVALO_IDENTITY_REFUSED, "switch after a factory reset");
    checked++;

    context() = "a claim of a new device ID starts its journal and keeps the boot counter";
    {
        V11_CHECK(store.initializeIdentity(OtherDeviceId), "claim initialization");
        NivaloJournal journal;
        V11_CHECK(store.loadJournal(&journal) == NIVALO_STORE_OK && std::strcmp(journal.deviceId, OtherDeviceId) == 0 &&
                      journal.highWater == 0U && !journal.hasLast,
                  "fresh journal");
        V11_CHECK(store.loadBoot(&counter) == NIVALO_STORE_OK && counter == 5U, "boot counter changed");
        NivaloExecutor executor(platform);
        boot(store, platform, executor, OtherDeviceId);
        executor.deliver(command(OtherDeviceId, 1U));
        V11_CHECK(platform.handlerCalls == 1, "first operation of the new identity");
        // An interrupted commit repeats the initialization: it keeps that history.
        V11_CHECK(store.initializeIdentity(OtherDeviceId) && store.loadJournal(&journal) == NIVALO_STORE_OK &&
                      journal.highWater == 1U,
                  "repeated initialization reset the journal");
    }
    checked++;
    return checked;
}

int floorCases()
{
    int checked = 0;
    nvs().reset();
    NivaloDeviceStore store;
    store.begin();
    uint32_t floor = 99U;

    context() = "floor: missing, valid, another product";
    V11_CHECK(store.loadFloor(ProductId, &floor) == NIVALO_STORE_MISSING && floor == 0U, "missing");
    V11_CHECK(store.writeFloor(ProductId, 9U) && store.loadFloor(ProductId, &floor) == NIVALO_STORE_OK && floor == 9U,
              "valid");
    V11_CHECK(store.loadFloor("99999999-9999-4999-8999-999999999999", &floor) == NIVALO_STORE_MISSING && floor == 0U,
              "another product's floor");
    checked++;

    context() = "an unreadable floor is reported as such and left in place";
    const std::vector<uint8_t> intact = nvs().entries["nivalo-dev/otaFloor"].bytes;
    nvs().entries["nivalo-dev/otaFloor"].bytes[20] ^= 0x01U;
    const std::vector<uint8_t> damaged = nvs().entries["nivalo-dev/otaFloor"].bytes;
    const size_t before = protocolWrites();
    V11_CHECK(store.loadFloor(ProductId, &floor) == NIVALO_STORE_CORRUPT && floor == 0U, "damaged floor read as zero");
    V11_CHECK(protocolWrites() == before && nvs().entries["nivalo-dev/otaFloor"].bytes == damaged, "record changed");
    nvs().entries["nivalo-dev/otaFloor"].bytes = intact;
    nvs().unreadable.insert("nivalo-dev/otaFloor");
    V11_CHECK(store.loadFloor(ProductId, &floor) == NIVALO_STORE_CORRUPT, "failed read taken as no floor");
    nvs().unreadable.clear();
    checked++;

    context() = "an unreadable catalog is not a first enrollment";
    NivaloCatalogState catalog;
    V11_CHECK(store.loadCatalog(&catalog) == NIVALO_STORE_MISSING, "missing catalog");
    catalog.revision = 8U;
    V11_CHECK(store.writeCatalog(catalog), "catalog write");
    nvs().unreadable.insert("nivalo-dev/catalog");
    V11_CHECK(store.loadCatalog(&catalog) == NIVALO_STORE_CORRUPT, "unreadable catalog read as missing");
    nvs().unreadable.clear();
    checked++;
    return checked;
}
} // namespace

int main()
{
    int checked = identityCases();
    checked += floorCases();
    return finish("protocol state store", checked);
}
