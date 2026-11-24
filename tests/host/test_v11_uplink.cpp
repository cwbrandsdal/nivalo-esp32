// The device's uplink builders (specs/cloud-mqtt-v1.1.md sections 3-8, 10.5, 12). Inputs the
// gateway would reject must be refused here; every accepted message is written to
// NIVALO_UPLINK_SAMPLES, which tests/validate_v11_uplinks.py replays through the protocol's
// reference model (tests/protocol_v1_1.py validate_message).

#include "NivaloCatalog.h"
#include "NivaloJournal.h"
#include "NivaloUplink.h"
#include "NivaloWire.h"
#include "v11_test_support.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using namespace v11test;

namespace
{
const char DeviceId[] = "22222222-2222-4222-8222-222222222222";
const int64_t SentAtMs = 1795521605000LL; // 2026-11-24T12:00:05.000Z
const char ReceivedAt[] = "2026-11-24T12:00:06Z";

struct Sample
{
    std::string topic;
    std::string payload;
};

std::vector<Sample> samples;
NivaloTopics topics;
uint32_t messageCounter = 0;

std::string nextMessageId()
{
    uint8_t random[16] = {0};
    const uint32_t counter = ++messageCounter;
    random[0] = 0x33;
    random[12] = (uint8_t)(counter >> 24);
    random[13] = (uint8_t)(counter >> 16);
    random[14] = (uint8_t)(counter >> 8);
    random[15] = (uint8_t)counter;
    char id[37];
    nivaloFormatUuidV4(random, id);
    return id;
}

bool emit(const char *topic, const char *payload, size_t length, const char *correlationId = NULL,
          const NivaloFirmwareInfo *firmware = NULL)
{
    char envelope[NivaloV11::MaxUplinkBytes + 64];
    const std::string messageId = nextMessageId();
    const size_t size = nivaloBuildEnvelope(DeviceId, messageId.c_str(), SentAtMs, correlationId, firmware, payload,
                                            length, envelope, sizeof(envelope));
    if (size == 0U)
    {
        return false;
    }
    V11_CHECK(size <= NivaloV11::MaxUplinkBytes, "envelope size " << size);
    samples.push_back(Sample{topic, std::string(envelope, size)});
    return true;
}

int availabilityCases()
{
    int checked = 0;
    char payload[512];
    NivaloEpoch epoch;
    epoch.boot = 41U;
    epoch.connection = 3U;
    NivaloFirmwareInfo firmware;
    firmware.version = "0.3.0";
    firmware.hardware = "esp32-standalone";

    context() = "birth";
    size_t length = nivaloBirthPayload(epoch, payload, sizeof(payload));
    V11_CHECK(length > 0 && emit(topics.availability(), payload, length, NULL, &firmware), "birth");
    checked++;

    context() = "heartbeat";
    NivaloHealth health;
    health.uptimeS = 610U;
    health.heapFreeB = 91040U;
    health.heapTotalB = 231424U;
    health.hasRssi = true;
    health.wifiRssiDbm = -67;
    length = nivaloHeartbeatPayload(epoch, 1U, health, payload, sizeof(payload));
    V11_CHECK(length > 0 && emit(topics.availability(), payload, length), "heartbeat");
    health.hasRssi = false;
    health.droppedBatches = 2U;
    length = nivaloHeartbeatPayload(epoch, 4294967294UL, health, payload, sizeof(payload));
    V11_CHECK(length > 0 && emit(topics.availability(), payload, length), "heartbeat without RSSI, last seq");
    checked += 2;

    context() = "heartbeat rejections";
    health.heapFreeB = health.heapTotalB + 1U;
    V11_CHECK(nivaloHeartbeatPayload(epoch, 2U, health, payload, sizeof(payload)) == 0U, "heap rule");
    health.heapFreeB = 1U;
    health.hasRssi = true;
    health.wifiRssiDbm = -128;
    V11_CHECK(nivaloHeartbeatPayload(epoch, 2U, health, payload, sizeof(payload)) == 0U, "rssi range");
    health.wifiRssiDbm = -1;
    V11_CHECK(nivaloHeartbeatPayload(epoch, 0U, health, payload, sizeof(payload)) == 0U, "seq 0 is the birth");
    NivaloEpoch none;
    V11_CHECK(nivaloBirthPayload(none, payload, sizeof(payload)) == 0U, "epoch zero");
    checked += 4;

    context() = "end";
    length = nivaloEndPayload(epoch, false, payload, sizeof(payload));
    V11_CHECK(length > 0 && emit(topics.availability(), payload, length), "will");
    length = nivaloEndPayload(epoch, true, payload, sizeof(payload));
    V11_CHECK(length > 0 && emit(topics.availability(), payload, length), "shutdown");
    checked += 2;

    context() = "firmware identity";
    NivaloFirmwareInfo bad;
    bad.version = " 0.3.0";
    bad.hardware = "esp32";
    V11_CHECK(!nivaloValidFirmwareInfo(bad), "leading space");
    const std::string tooLong(65, 'v');
    bad.version = tooLong.c_str();
    V11_CHECK(!nivaloValidFirmwareInfo(bad), "65 bytes");
    checked += 2;
    return checked;
}

int ackCases()
{
    int checked = 0;
    struct Combination
    {
        NivaloAckStatus status;
        NivaloAckCode code;
        const char *message;
        int progress;
    };
    const Combination Valid[] = {
        {NIVALO_ACK_ACCEPTED, NIVALO_CODE_NONE, NULL, -1},
        {NIVALO_ACK_RUNNING, NIVALO_CODE_NONE, "Downloading firmware", 45},
        {NIVALO_ACK_RUNNING, NIVALO_CODE_NONE, NULL, -1},
        {NIVALO_ACK_SUCCEEDED, NIVALO_CODE_NONE, "done", -1},
        {NIVALO_ACK_FAILED, NIVALO_CODE_EXPIRED, NULL, -1},
        {NIVALO_ACK_FAILED, NIVALO_CODE_CLOCK_UNSYNCED, NULL, -1},
        {NIVALO_ACK_FAILED, NIVALO_CODE_INVALID_COMMAND, NULL, -1},
        {NIVALO_ACK_FAILED, NIVALO_CODE_NOT_ALLOWED, NULL, -1},
        {NIVALO_ACK_FAILED, NIVALO_CODE_ID_CONFLICT, NULL, -1},
        {NIVALO_ACK_FAILED, NIVALO_CODE_STORAGE_ERROR, NULL, -1},
        {NIVALO_ACK_FAILED, NIVALO_CODE_OTA_REJECTED, NULL, -1},
        {NIVALO_ACK_FAILED, NIVALO_CODE_HANDLER_FAILED, NULL, -1},
        {NIVALO_ACK_INDETERMINATE, NIVALO_CODE_EXECUTION_UNKNOWN, NULL, -1},
        {NIVALO_ACK_INDETERMINATE, NIVALO_CODE_REPLAY_SUPPRESSED, NULL, -1},
    };
    const char *ids[2] = {"44444444-4444-4444-8444-44440000002a", "55555555-5555-4555-8555-55550000002a"};
    for (int route = 0; route < 2; route++)
    {
        char topic[160];
        topics.ack(route == 0 ? NIVALO_OPERATION_COMMAND : NIVALO_OPERATION_OTA, ids[route], topic, sizeof(topic));
        for (const Combination &combination : Valid)
        {
            context() = std::string("ack ") + nivaloAckStatusName(combination.status) + " " +
                        (nivaloAckCodeName(combination.code) ? nivaloAckCodeName(combination.code) : "");
            char payload[256];
            const size_t length = nivaloAckPayload(42U, nivaloResult(combination.status, combination.code),
                                                   combination.message, combination.progress, payload, sizeof(payload));
            V11_CHECK(length > 0 && emit(topic, payload, length, ids[route]), "valid ACK");
            checked++;
        }
    }
    context() = "ack rejections";
    char payload[256];
    V11_CHECK(nivaloAckPayload(42U, nivaloResult(NIVALO_ACK_FAILED), NULL, -1, payload, sizeof(payload)) == 0U, "failed needs a code");
    V11_CHECK(nivaloAckPayload(42U, nivaloResult(NIVALO_ACK_SUCCEEDED, NIVALO_CODE_EXPIRED), NULL, -1, payload, sizeof(payload)) == 0U,
              "succeeded has no code");
    V11_CHECK(nivaloAckPayload(42U, nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_EXECUTION_UNKNOWN), NULL, -1, payload, sizeof(payload)) == 0U,
              "failed with an uncertainty code");
    V11_CHECK(nivaloAckPayload(42U, nivaloResult(NIVALO_ACK_SUCCEEDED), NULL, 10, payload, sizeof(payload)) == 0U,
              "progress only on running");
    V11_CHECK(nivaloAckPayload(42U, nivaloResult(NIVALO_ACK_SUCCEEDED), "done ", -1, payload, sizeof(payload)) == 0U,
              "message text rules");
    V11_CHECK(nivaloAckPayload(42U, nivaloResult(NIVALO_ACK_SUCCEEDED), std::string(96, 'm').c_str(), -1, payload, sizeof(payload)) == 0U,
              "message of 96 bytes");
    checked += 6;
    return checked;
}

bool publishBatch(NivaloTelemetryBatch &batch)
{
    size_t length = 0;
    const char *payload = batch.payload(&length);
    const bool published = payload != NULL && emit(topics.telemetry(), payload, length);
    batch.reset();
    return published;
}

int telemetryCases()
{
    int checked = 0;
    NivaloTelemetryBatch batch;
    const int64_t base = SentAtMs - 60000;

    context() = "single reading";
    V11_CHECK(batch.beginSample(base) && batch.addValue("temp", 21.125, "C") && publishBatch(batch), "single");
    checked++;

    context() = "raw values are preserved";
    V11_CHECK(batch.beginSample(base) && batch.addRaw("door", " open ", NULL) && batch.addRaw("note", "", NULL) &&
                  batch.addRaw("label", "line\nbreak \"quoted\"", "text") && publishBatch(batch),
              "raw");
    checked++;

    context() = "24 readings in 8 samples";
    bool ok = true;
    for (int sample = 0; sample < 8; sample++)
    {
        ok = ok && batch.beginSample(base + sample * 1000);
        for (int metric = 0; metric < 3; metric++)
        {
            ok = ok && batch.addValue(("m" + std::to_string(metric)).c_str(), sample * 10 + metric, "u");
        }
    }
    V11_CHECK(ok && batch.readings() == 24U, "full batch");
    V11_CHECK(!batch.addValue("extra", 1, NULL), "25th reading");
    V11_CHECK(!batch.beginSample(base + 9000), "9th sample");
    V11_CHECK(publishBatch(batch), "publish full batch");
    checked += 4;

    context() = "sample rules";
    V11_CHECK(batch.beginSample(base) && batch.addValue("temp", 1, "C"), "first");
    V11_CHECK(!batch.addValue("temp", 2, "C"), "duplicate name in a sample");
    V11_CHECK(!batch.addValue("TEMP", 2, "C"), "case twin in a sample");
    V11_CHECK(!batch.beginSample(base), "sample times strictly increase");
    V11_CHECK(batch.beginSample(base + 1) && batch.addValue("temp", 2, "C"), "a name may repeat in a later sample");
    V11_CHECK(batch.beginSample(base + 2) && batch.beginSample(base + 3) && batch.addValue("temp", 3, "C"),
              "an empty sample moves to the next time");
    V11_CHECK(batch.beginSample(base + 4) && publishBatch(batch), "a trailing empty sample is dropped");
    V11_CHECK(!batch.addValue("temp", 1, "C"), "a metric needs an open sample");
    checked += 8;

    context() = "exact decimals";
    const double values[] = {-0.000001, 4294967295.999999, -4294967295.999999, 0.1f, 1e-7, -0.0, 123456.5};
    V11_CHECK(batch.beginSample(base), "sample");
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        V11_CHECK(batch.addValue(("v" + std::to_string(i)).c_str(), values[i], NULL), "value " << values[i]);
    }
    V11_CHECK(publishBatch(batch), "publish decimals");
    char text[32];
    V11_CHECK(nivaloFormatDecimal(0.1f, text, sizeof(text)) && std::string(text) == "0.1", text);
    V11_CHECK(nivaloFormatDecimal(-0.0, text, sizeof(text)) && std::string(text) == "0", text);
    V11_CHECK(nivaloFormatDecimal(21.125, text, sizeof(text)) && std::string(text) == "21.125", text);
    V11_CHECK(batch.beginSample(base), "sample");
    V11_CHECK(!batch.addValue("big", 4294967296.0, NULL), "out of range");
    V11_CHECK(!batch.addValue("nan", std::numeric_limits<double>::quiet_NaN(), NULL), "NaN");
    V11_CHECK(!batch.addValue("inf", std::numeric_limits<double>::infinity(), NULL), "infinity");
    V11_CHECK(!batch.addValue("9name", 1, NULL), "name pattern");
    V11_CHECK(!batch.addValue("ok", 1, " C"), "unit text rules");
    V11_CHECK(!batch.addRaw("raw", std::string(513, 'r').c_str(), NULL), "raw longer than 512 bytes");
    V11_CHECK(!batch.addRaw("raw", "\xC3\x28", NULL), "raw must be UTF-8");
    batch.reset();
    checked += 15;

    context() = "size limit";
    const std::string unit(32, 'u');
    const std::string raw(512, 'r');
    unsigned accepted = 0;
    V11_CHECK(batch.beginSample(base), "sample");
    for (int i = 0; i < 24; i++)
    {
        std::string name = std::string(60, 'n') + std::to_string(1000 + i);
        if (batch.addRaw(name.c_str(), raw.c_str(), unit.c_str()))
        {
            accepted++;
        }
    }
    V11_CHECK(accepted >= 3U && accepted < 24U, "accepted " << accepted);
    V11_CHECK(publishBatch(batch), "the largest batch still fits its envelope");
    checked += 3;
    return checked;
}

int eventCases()
{
    int checked = 0;
    char payload[NivaloV11::MaxUplinkBytes];
    struct Data
    {
        const char *json;
        bool valid;
    };
    const Data Cases[] = {
        {"{\"zone\":1,\"open\":true}", true},
        {"[1,2,{\"a\":null}]", true},
        {"\"text\"", true},
        {"null", true},
        {"-1.5e3", true},
        {"[[[[[[1]]]]]]", true},
        {"[[[[[[[1]]]]]]]", false},
        {"{\"a\":1,\"a\":2}", false},
        {"{a:1}", false},
        {"{\"a\":\"\\u0000\"}", false},
        {"NaN", false},
    };
    for (const Data &data : Cases)
    {
        context() = std::string("event data ") + data.json;
        const size_t length = nivaloEventPayload("door.opened", "info", data.json, NULL, payload, sizeof(payload));
        V11_CHECK((length > 0) == data.valid, "length " << length);
        if (length > 0)
        {
            V11_CHECK(emit(topics.events(), payload, length), "event envelope");
        }
        checked++;
    }
    context() = "event raw and severities";
    const char *severities[] = {"debug", "info", "warning", "error", "critical"};
    for (const char *severity : severities)
    {
        const size_t length = nivaloEventPayload("alarm", severity, NULL, "raw text", payload, sizeof(payload));
        V11_CHECK(length > 0 && emit(topics.events(), payload, length), severity);
        checked++;
    }
    size_t length = nivaloEventPayload("boot", "info", NULL, NULL, payload, sizeof(payload));
    V11_CHECK(length > 0 && emit(topics.events(), payload, length), "no data");
    V11_CHECK(nivaloEventPayload("state.door", "info", NULL, NULL, payload, sizeof(payload)) == 0U, "state. prefix");
    V11_CHECK(nivaloEventPayload("STATE.door", "info", NULL, NULL, payload, sizeof(payload)) == 0U, "STATE. prefix");
    V11_CHECK(nivaloEventPayload("door", "notice", NULL, NULL, payload, sizeof(payload)) == 0U, "severity");
    V11_CHECK(nivaloEventPayload("door", "info", "1", "raw", payload, sizeof(payload)) == 0U, "data and raw");
    V11_CHECK(nivaloEventPayload("door", "info", NULL, std::string(1025, 'r').c_str(), payload, sizeof(payload)) == 0U,
              "raw longer than 1024 bytes");
    checked += 6;

    context() = "state";
    const char *values[] = {"null", "{\"mode\":\"auto\"}", "21.5", "\"on\""};
    for (const char *value : values)
    {
        length = nivaloStatePayload("mode", value, payload, sizeof(payload));
        V11_CHECK(length > 0 && emit(topics.state(), payload, length), value);
        checked++;
    }
    V11_CHECK(nivaloStatePayload("mode", "{\"a\":1,\"a\":1}", payload, sizeof(payload)) == 0U, "duplicate member");
    checked++;
    return checked;
}

class Registered : public NivaloCatalogSource
{
public:
    size_t capabilityCount() const override { return 3U; }
    NivaloCapabilityKind capabilityKind(size_t index) const override
    {
        return index == 2U ? NIVALO_KIND_FUNCTION : NIVALO_KIND_VARIABLE;
    }
    const char *capabilityName(size_t index) const override
    {
        static const char *const Names[] = {"ledState", "temp", "setLed"};
        return Names[index];
    }
    void writeDefinition(size_t index, NivaloJsonWriter &writer) const override
    {
        writer.beginObject();
        if (index < 2U)
        {
            writer.key("dataType");
            writer.string(index == 0U ? "boolean" : "number");
            if (index == 1U)
            {
                writer.key("unit");
                writer.string("C");
            }
            writer.key("historyEnabled");
            writer.boolean(true);
        }
        else
        {
            writer.key("displayName");
            writer.string("Set LED");
            writer.key("argumentExample");
            writer.string("{\"state\":true}");
            writer.key("returnType");
            writer.string("void");
            writer.key("timeoutSeconds");
            writer.uint(30U);
            writer.key("dangerLevel");
            writer.string("operational");
        }
        writer.key("sortOrder");
        writer.uint(index);
        writer.endObject();
    }
};

class Publisher : public NivaloCatalogSink
{
public:
    int packets = 0;
    bool emitPacket(const char *payload, size_t length) override
    {
        packets++;
        return emit(topics.definitions(), payload, length);
    }
};

int definitionCases()
{
    context() = "definitions";
    Registered source;
    NivaloCatalogState state;
    V11_CHECK(nivaloPlanCatalog(source, NULL, &state) == NIVALO_CATALOG_CHANGED, "plan");
    Publisher publisher;
    V11_CHECK(nivaloRefreshCatalog(source, state, NivaloTelemetryBatch::PayloadCapacity, publisher) &&
                  publisher.packets == 1,
              "refresh");
    return 1;
}

void writeSamples()
{
    const char *path = std::getenv("NIVALO_UPLINK_SAMPLES");
    if (path == NULL)
    {
        return;
    }
    DynamicJsonDocument document(samples.size() * 4096U + 4096U);
    JsonArray array = document.to<JsonArray>();
    for (const Sample &sample : samples)
    {
        JsonObject entry = array.createNestedObject();
        entry["topic"] = sample.topic.c_str();
        entry["payload"] = sample.payload.c_str();
        entry["receivedAt"] = ReceivedAt;
    }
    std::FILE *file = std::fopen(path, "wb");
    if (file == NULL)
    {
        V11_CHECK(false, "cannot write " << path);
        return;
    }
    std::string text;
    serializeJson(document, text);
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    std::printf("wrote %u uplink samples to %s\n", (unsigned)samples.size(), path);
}
} // namespace

int main()
{
    topics.configure(DeviceId);
    int checked = availabilityCases();
    checked += ackCases();
    checked += telemetryCases();
    checked += eventCases();
    checked += definitionCases();
    writeSamples();
    return finish("uplink builders", checked);
}
