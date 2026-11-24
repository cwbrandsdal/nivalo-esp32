// Replays nivalo-protocol fixtures/v1.1/vectors/command-execution.json against the firmware's
// downlink parser, executor and journal (specs/cloud-mqtt-v1.1.md sections 9, 10.4 and 11.4).
// Power loss is simulated by an exception thrown from the fake storage; the durable state of the
// fake survives it and the next boot step builds a new executor from that state, as a reset would.

#include "NivaloDownlink.h"
#include "NivaloExecutor.h"
#include "NivaloJournal.h"
#include "NivaloOtaPolicy.h"
#include "v11_test_support.h"

#include <cmath>
#include <cstring>
#include <map>
#include <vector>

using namespace v11test;

namespace
{
const char DeviceId[] = "22222222-2222-4222-8222-222222222222";
const int64_t MonotonicBaseUs = 5000000000LL;

struct PowerCut
{
};

int64_t utc(JsonVariantConst value)
{
    int64_t ms = 0;
    if (!nivaloParseUtc(str(value), &ms))
    {
        V11_CHECK(false, "bad fixture timestamp " << str(value));
    }
    return ms;
}

int64_t monotonic(JsonVariantConst elapsedSeconds)
{
    return MonotonicBaseUs + (int64_t)std::llround(elapsedSeconds.as<double>() * 1000000.0);
}

NivaloClockReading reading(JsonVariantConst spec, bool defaultValid = true)
{
    NivaloClockReading result;
    result.valid = spec["valid"].isNull() ? defaultValid : spec["valid"].as<bool>();
    result.utcMs = spec["now"].isNull() ? 0 : utc(spec["now"]);
    result.monotonicUs = spec["elapsed"].isNull() ? MonotonicBaseUs : monotonic(spec["elapsed"]);
    return result;
}

// Enough JSON Schema for the vector's handler argument schemas.
bool schemaAccepts(JsonVariantConst schema, JsonVariantConst value)
{
    const char *type = str(schema["type"], NULL);
    if (type != NULL)
    {
        const std::string t(type);
        if ((t == "object" && !value.is<JsonObjectConst>()) || (t == "boolean" && !value.is<bool>()) ||
            (t == "string" && !value.is<const char *>()) || (t == "integer" && !value.is<long long>()) ||
            (t == "number" && !(value.is<double>() || value.is<long long>())) ||
            (t == "array" && !value.is<JsonArrayConst>()))
        {
            return false;
        }
    }
    if (!value.is<JsonObjectConst>())
    {
        return true;
    }
    JsonObjectConst object = value.as<JsonObjectConst>();
    for (JsonVariantConst required : schema["required"].as<JsonArrayConst>())
    {
        if (!object.containsKey(str(required)))
        {
            return false;
        }
    }
    JsonObjectConst properties = schema["properties"].as<JsonObjectConst>();
    for (JsonPairConst member : object)
    {
        if (!properties.isNull() && properties.containsKey(member.key().c_str()))
        {
            if (!schemaAccepts(properties[member.key().c_str()], member.value()))
            {
                return false;
            }
        }
        else if (schema["additionalProperties"].is<bool>() && !schema["additionalProperties"].as<bool>())
        {
            return false;
        }
    }
    return true;
}

enum Stage
{
    STAGE_IDLE,
    STAGE_CLAIMED,
    STAGE_DOWNLOADED,
    STAGE_PENDING
};

class FakeDevice : public NivaloExecutorPlatform
{
public:
    // Durable state, kept across simulated resets.
    bool journalStored = false;
    NivaloJournal journal;
    bool pendingStored = false;
    NivaloPendingRecord pending;
    uint32_t floor = 0U;

    // Effects observed during one step.
    std::vector<NivaloAck> acks;
    int handlerCalls = 0;
    int refreshes = 0;
    int activations = 0;

    // Behaviour of the current step.
    std::string handler;
    JsonObjectConst functions;
    JsonVariantConst clock;
    Stage stage = STAGE_IDLE;
    std::vector<std::pair<int64_t, int64_t>> syncs;
    bool booting = false;
    std::string interrupt;
    std::string slot;
    bool hasSlot = false;
    NivaloExecutor *executor = NULL;
    NivaloOperationClass activeClass = NIVALO_OPERATION_COMMAND;

    void beginStep(JsonVariantConst step)
    {
        acks.clear();
        handlerCalls = refreshes = activations = 0;
        handler = str(step["handler"]);
        clock = step["clock"];
        // Synchronisations queued in an earlier step and not yet folded stay queued, as on the
        // device, where the SNTP callback queue is drained at the next clock reading.
    }

    void queueSyncs()
    {
        for (JsonVariantConst sync : clock["syncs"].as<JsonArrayConst>())
        {
            syncs.push_back(std::make_pair(utc(sync["now"]), monotonic(sync["elapsed"])));
        }
    }

    bool writeJournal(const NivaloJournal &record) override
    {
        const bool claim = record.hasLast && record.phase == NIVALO_PHASE_CLAIMED && !booting;
        if (claim && handler == "powerLossBeforeClaim")
        {
            throw PowerCut();
        }
        // The production store encodes, writes and reads back; so does the fake.
        uint8_t blob[NivaloJournalBlobBytes];
        const size_t length = nivaloEncodeJournal(record, blob, sizeof(blob));
        NivaloJournal decoded;
        V11_CHECK(length > 0U && nivaloDecodeJournal(blob, length, &decoded), "journal codec round trip");
        journal = decoded;
        journalStored = true;
        if (claim)
        {
            stage = STAGE_CLAIMED;
            activeClass = (NivaloOperationClass)record.operationClass;
            queueSyncs();
            if (handler == "powerLossAfterClaim")
            {
                throw PowerCut();
            }
        }
        if (booting && interrupt == "afterJournal")
        {
            throw PowerCut();
        }
        return true;
    }

    bool writePending(const NivaloPendingRecord &record) override
    {
        uint8_t blob[NivaloPendingBlobBytes];
        const size_t length = nivaloEncodePending(record, blob, sizeof(blob));
        NivaloPendingRecord decoded;
        V11_CHECK(length > 0U && nivaloDecodePending(blob, length, &decoded), "pending codec round trip");
        pending = decoded;
        pendingStored = true;
        stage = STAGE_PENDING;
        if (handler == "powerLossAfterPending")
        {
            throw PowerCut();
        }
        return true;
    }

    bool clearPending() override
    {
        pendingStored = false;
        return true;
    }

    bool writeFloor(uint32_t value) override
    {
        floor = value;
        if (booting && interrupt == "afterFloor")
        {
            throw PowerCut();
        }
        return true;
    }

    NivaloClockReading readClock() override
    {
        for (size_t i = 0; i < syncs.size(); i++)
        {
            executor->onTrustedSync(syncs[i].first, syncs[i].second);
        }
        syncs.clear();
        NivaloClockReading claim = reading(clock);
        switch (stage)
        {
        case STAGE_IDLE:
            return claim;
        case STAGE_CLAIMED:
            if (activeClass == NIVALO_OPERATION_COMMAND)
            {
                // A generic handler starts at the claim instant unless the step gives a reading.
                return clock["start"].isNull() ? claim : reading(clock["start"]);
            }
            return claim;
        case STAGE_DOWNLOADED:
            return clock["downloaded"].isNull() ? claim : reading(clock["downloaded"]);
        case STAGE_PENDING:
            V11_CHECK(!clock["start"].isNull(), "an OTA activation needs a fresh reading after the pending write");
            return reading(clock["start"]);
        }
        return claim;
    }

    void publishAck(const NivaloAck &ack) override { acks.push_back(ack); }

    bool hasFunction(const char *name) override { return functions.containsKey(name); }

    bool validArguments(const char *name, const char *argumentsJson) override
    {
        DynamicJsonDocument arguments(4096);
        if (deserializeJson(arguments, argumentsJson) != DeserializationError::Ok)
        {
            return false;
        }
        return schemaAccepts(functions[name], arguments.as<JsonVariantConst>());
    }

    NivaloFunctionOutcome invokeFunction(const char *, const char *) override
    {
        handlerCalls++;
        return outcome(handler);
    }

    NivaloFunctionOutcome outcome(const std::string &name)
    {
        if (name == "powerLossAfterEffect")
        {
            throw PowerCut();
        }
        if (name == "runs")
        {
            return NIVALO_FUNCTION_PENDING;
        }
        if (name == "failed")
        {
            return NIVALO_FUNCTION_FAILED;
        }
        if (name == "unknown")
        {
            return NIVALO_FUNCTION_UNKNOWN;
        }
        V11_CHECK(name.empty() || name == "succeeded", "unknown handler outcome " << name);
        return NIVALO_FUNCTION_SUCCEEDED;
    }

    void refreshCatalog() override { refreshes++; }

    NivaloDownloadOutcome downloadImage(const NivaloOperation &) override { return download(handler); }

    NivaloDownloadOutcome download(const std::string &name)
    {
        if (name == "downloading")
        {
            return NIVALO_DOWNLOAD_PENDING;
        }
        if (name == "powerLossDuringDownload")
        {
            throw PowerCut();
        }
        if (name == "imageRejected")
        {
            return NIVALO_DOWNLOAD_REJECTED;
        }
        stage = STAGE_DOWNLOADED;
        return NIVALO_DOWNLOAD_VERIFIED;
    }

    NivaloActivationOutcome activateImage() override
    {
        activations++;
        if (handler == "activationRejected")
        {
            return NIVALO_ACTIVATION_REJECTED;
        }
        if (handler == "activationUncertain")
        {
            return NIVALO_ACTIVATION_UNCERTAIN;
        }
        return NIVALO_ACTIVATION_SELECTED;
    }

    bool hashRunningSlot(uint32_t sizeBytes, char output[65]) override
    {
        if (!hasSlot || slot.size() < sizeBytes)
        {
            return false;
        }
        const std::string digest = sha256Hex(slot.substr(0, sizeBytes));
        std::memcpy(output, digest.c_str(), 65U);
        return true;
    }

    void restart() override { throw PowerCut(); }
};

std::string ackText(const NivaloAck &ack)
{
    std::ostringstream text;
    text << "{" << ack.seq << " " << nivaloAckStatusName(ack.result.status);
    if (ack.result.code != NIVALO_CODE_NONE)
    {
        text << " " << nivaloAckCodeName(ack.result.code);
    }
    if (ack.deferred)
    {
        text << " deferred";
    }
    text << "}";
    return text.str();
}

std::string expectedAckText(JsonVariantConst ack)
{
    std::ostringstream text;
    text << "{" << ack["commandSeq"].as<unsigned long>() << " " << str(ack["status"]);
    if (!ack["code"].isNull())
    {
        text << " " << str(ack["code"]);
    }
    if (ack["deferred"].is<bool>() && ack["deferred"].as<bool>())
    {
        text << " deferred";
    }
    text << "}";
    return text.str();
}

std::string resultText(const NivaloResult &result)
{
    std::string text = nivaloAckStatusName(result.status) == NULL ? "?" : nivaloAckStatusName(result.status);
    if (result.code != NIVALO_CODE_NONE)
    {
        text += std::string("/") + nivaloAckCodeName(result.code);
    }
    return text;
}

std::string expectedResultText(JsonVariantConst result)
{
    std::string text = str(result["status"]);
    if (!result["code"].isNull())
    {
        text += std::string("/") + str(result["code"]);
    }
    return text;
}

NivaloResult resultFrom(JsonVariantConst result)
{
    static const char *const Statuses[] = {"", "accepted", "running", "succeeded", "failed", "indeterminate"};
    NivaloResult value;
    for (uint8_t i = 1; i <= 5; i++)
    {
        if (std::strcmp(str(result["status"]), Statuses[i]) == 0)
        {
            value.status = i;
        }
    }
    for (uint8_t code = NIVALO_CODE_EXPIRED; code <= NIVALO_CODE_REPLAY_SUPPRESSED; code++)
    {
        if (!result["code"].isNull() && std::strcmp(str(result["code"]), nivaloAckCodeName(code)) == 0)
        {
            value.code = code;
        }
    }
    return value;
}

void hexToBytes(const std::string &hex, uint8_t *output)
{
    for (size_t i = 0; i < hex.size() / 2U; i++)
    {
        output[i] = (uint8_t)std::strtoul(hex.substr(i * 2U, 2U).c_str(), NULL, 16);
    }
}

NivaloPendingRecord pendingFrom(JsonVariantConst value)
{
    NivaloPendingRecord record;
    record.commandSeq = value["commandSeq"].as<uint32_t>();
    std::strncpy(record.productId, str(value["productId"]), 36U);
    record.securityVersion = value["securityVersion"].as<uint32_t>();
    record.sizeBytes = value["sizeBytes"].as<uint32_t>();
    std::strncpy(record.sha256, str(value["sha256"]), 64U);
    return record;
}

bool samePending(const NivaloPendingRecord &left, const NivaloPendingRecord &right)
{
    return left.commandSeq == right.commandSeq && std::strcmp(left.productId, right.productId) == 0 &&
           left.securityVersion == right.securityVersion && left.sizeBytes == right.sizeBytes &&
           std::strcmp(left.sha256, right.sha256) == 0;
}

std::string envelopeFor(const std::string &correlationId, const std::string &payloadText)
{
    return std::string("{\"schema\":\"nivalo.iot.v1.1\",\"messageId\":\"33333333-3333-4333-8333-3333000000ee\",") +
           "\"deviceId\":\"" + DeviceId + "\",\"sentAt\":\"2026-11-24T12:00:05Z\",\"correlationId\":\"" +
           correlationId + "\",\"payload\":" + payloadText + "}";
}

struct Harness
{
    FakeDevice device;
    std::unique_ptr<NivaloExecutor> executor;
    NivaloOtaDevice ota;
    std::vector<NivaloFirmwareSigningKey> keys;
    std::vector<std::string> pems;
    std::vector<std::string> keyIds;
    std::string productId, apiOrigin;
    NivaloImageIdentity runningImage;
    NivaloTopics topics;
    bool powered = true;

    Harness(JsonObjectConst root, JsonObjectConst testCase)
    {
        device.functions = root["functions"].as<JsonObjectConst>();
        JsonObjectConst otaSpec = root["ota"].as<JsonObjectConst>();
        productId = str(otaSpec["productId"]);
        apiOrigin = str(otaSpec["apiOrigin"]);
        for (JsonPairConst key : otaSpec["trustedKeys"].as<JsonObjectConst>())
        {
            keyIds.push_back(key.key().c_str());
            pems.push_back(pemFromSpkiBase64(str(key.value())));
        }
        for (size_t i = 0; i < pems.size(); i++)
        {
            NivaloFirmwareSigningKey key = {keyIds[i].c_str(), pems[i].c_str()};
            keys.push_back(key);
        }
        ota.keys = keys.data();
        ota.keyCount = keys.size();
        ota.productId = productId.c_str();
        ota.runningSecurityVersion = otaSpec["compiledSecurityVersion"].as<uint32_t>();
        ota.committedFloor = otaSpec["committedFloor"].as<uint32_t>();
        ota.maxImageBytes = otaSpec["maxImageBytes"].as<uint32_t>();
        ota.apiOrigin = apiOrigin.c_str();
        device.floor = ota.committedFloor;
        std::strncpy(runningImage.productId, productId.c_str(), 36U);
        runningImage.securityVersion = ota.runningSecurityVersion;
        topics.configure(DeviceId);

        JsonObjectConst journalSpec = testCase["journal"].as<JsonObjectConst>();
        NivaloJournal journal = nivaloFreshJournal(DeviceId);
        journal.highWater = journalSpec["highWater"].as<uint32_t>();
        JsonObjectConst last = journalSpec["last"].as<JsonObjectConst>();
        if (!last.isNull())
        {
            journal.hasLast = true;
            journal.seq = last["seq"].as<uint32_t>();
            std::strncpy(journal.commandId, str(last["commandId"]), 36U);
            journal.operationClass =
                std::strcmp(str(last["class"]), "ota") == 0 ? NIVALO_OPERATION_OTA : NIVALO_OPERATION_COMMAND;
            const std::string text = last["payloadText"].is<const char *>() ? std::string(str(last["payloadText"]))
                                                                             : compact(last["payload"]);
            hexToBytes(sha256Hex(text), journal.fingerprint);
            journal.phase = std::strcmp(str(last["phase"]), "claimed") == 0 ? NIVALO_PHASE_CLAIMED : NIVALO_PHASE_DONE;
            if (journal.phase == NIVALO_PHASE_DONE)
            {
                journal.result = resultFrom(last["result"]);
            }
        }
        device.journal = journal;
        device.journalStored = true;
        if (!testCase["pending"].isNull())
        {
            device.pending = pendingFrom(testCase["pending"]);
            device.pendingStored = true;
        }
        if (journal.hasLast && journal.phase == NIVALO_PHASE_CLAIMED)
        {
            // An operation was in flight when the case starts: the first step is the boot that
            // reconciles it.
            powered = false;
            return;
        }
        // The device as it is at the start of the case: already booted and connected.
        boot(NULL, true);
    }

    void boot(const NivaloImageIdentity *image, bool initial)
    {
        executor.reset(new NivaloExecutor(device));
        device.executor = executor.get();
        device.stage = STAGE_IDLE;
        ota.committedFloor = device.floor;
        if (image != NULL)
        {
            ota.runningSecurityVersion = image->securityVersion;
        }
        device.booting = true;
        NivaloJournal journal = device.journal;
        NivaloPendingRecord pending = device.pending;
        try
        {
            executor->begin(DeviceId, device.journalStored ? &journal : NULL, device.pendingStored ? &pending : NULL,
                            &ota, image != NULL ? *image : runningImage);
            powered = true;
        }
        catch (const PowerCut &)
        {
            powered = false;
        }
        device.booting = false;
        device.interrupt.clear();
        if (initial)
        {
            device.acks.clear();
        }
    }

    void deliver(JsonVariantConst step)
    {
        const bool ota = std::strcmp(str(step["route"]), "ota") == 0;
        const std::string commandId = str(step["commandId"]);
        const std::string correlationId = step["correlationId"].is<const char *>() ? std::string(str(step["correlationId"]))
                                                                                    : commandId;
        const std::string payloadText = step["payloadText"].is<const char *>() ? std::string(str(step["payloadText"]))
                                                                                : compact(step["payload"]);
        const std::string topic = std::string("nivalo/v1.1/devices/") + DeviceId + (ota ? "/ota/" : "/commands/") + commandId;
        const std::string envelope = envelopeFor(correlationId, payloadText);
        NivaloOperation operation;
        const NivaloDownlinkOutcome outcome =
            nivaloParseDownlink(topics, topic.c_str(), reinterpret_cast<const uint8_t *>(envelope.data()),
                                envelope.size(), &operation);
        if (outcome != NIVALO_DOWNLINK_IDENTIFIED)
        {
            return;
        }
        char fingerprint[65];
        nivaloHexEncode(operation.fingerprint, 32U, fingerprint);
        V11_CHECK(sha256Hex(payloadText) == fingerprint, "fingerprint is not the SHA-256 of the payload bytes");
        executor->deliver(operation);
    }

    void complete()
    {
        if (device.activeClass == NIVALO_OPERATION_OTA)
        {
            device.queueSyncs();
            executor->completeDownload(device.download(device.handler.empty() ? "installed" : device.handler));
        }
        else
        {
            executor->completeFunction(device.outcome(device.handler));
        }
    }
};

void checkExpectations(Harness &harness, JsonObjectConst expect)
{
    FakeDevice &device = harness.device;
    std::string actualAcks, expectedAcks;
    for (size_t i = 0; i < device.acks.size(); i++)
    {
        actualAcks += ackText(device.acks[i]);
    }
    for (JsonVariantConst ack : expect["acks"].as<JsonArrayConst>())
    {
        expectedAcks += expectedAckText(ack);
    }
    V11_CHECK(actualAcks == expectedAcks, "acks " << actualAcks << " expected " << expectedAcks);
    if (!expect["handlerCalls"].isNull())
    {
        V11_CHECK(device.handlerCalls == expect["handlerCalls"].as<int>(),
                  "handlerCalls " << device.handlerCalls << " expected " << expect["handlerCalls"].as<int>());
    }
    if (!expect["catalogRefreshes"].isNull())
    {
        V11_CHECK(device.refreshes == expect["catalogRefreshes"].as<int>(), "catalogRefreshes " << device.refreshes);
    }
    if (!expect["otaActivations"].isNull())
    {
        V11_CHECK(device.activations == expect["otaActivations"].as<int>(), "otaActivations " << device.activations);
    }
    JsonObjectConst journal = expect["journal"].as<JsonObjectConst>();
    if (!journal.isNull())
    {
        V11_CHECK(device.journal.highWater == journal["highWater"].as<uint32_t>(),
                  "highWater " << device.journal.highWater << " expected " << journal["highWater"].as<uint32_t>());
        if (journal.containsKey("phase"))
        {
            const char *phase = device.journal.phase == NIVALO_PHASE_CLAIMED ? "claimed" : "done";
            V11_CHECK(device.journal.hasLast && std::strcmp(phase, str(journal["phase"])) == 0,
                      "phase " << phase << " expected " << str(journal["phase"]));
            if (journal["result"].isNull())
            {
                V11_CHECK(device.journal.phase == NIVALO_PHASE_CLAIMED, "a claimed record has no result");
            }
            else
            {
                V11_CHECK(resultText(device.journal.result) == expectedResultText(journal["result"]),
                          "result " << resultText(device.journal.result) << " expected "
                                    << expectedResultText(journal["result"]));
            }
        }
        else
        {
            V11_CHECK(!device.journal.hasLast, "unexpected journal record");
        }
    }
    if (expect.containsKey("pending"))
    {
        if (expect["pending"].isNull())
        {
            V11_CHECK(!device.pendingStored, "pending record should be cleared");
        }
        else
        {
            V11_CHECK(device.pendingStored && samePending(device.pending, pendingFrom(expect["pending"])),
                      "pending record differs");
        }
    }
    if (!expect["floor"].isNull())
    {
        V11_CHECK(device.floor == expect["floor"].as<uint32_t>(),
                  "floor " << device.floor << " expected " << expect["floor"].as<uint32_t>());
    }
}

NivaloImageIdentity imageFrom(JsonVariantConst spec, FakeDevice &device)
{
    NivaloImageIdentity image;
    std::strncpy(image.productId, str(spec["productId"]), 36U);
    image.securityVersion = spec["securityVersion"].as<uint32_t>();
    device.slot = std::string(str(spec["slotAscii"])) + std::string(spec["erasedTailBytes"].as<size_t>(), '\xff');
    device.hasSlot = true;
    return image;
}

int runCases(JsonObjectConst root)
{
    int checked = 0;
    for (JsonObjectConst testCase : root["cases"].as<JsonArrayConst>())
    {
        Harness harness(root, testCase);
        int stepIndex = 0;
        for (JsonObjectConst step : testCase["steps"].as<JsonArrayConst>())
        {
            context() = std::string(str(testCase["id"])) + " step " + std::to_string(stepIndex++) + " " + str(step["action"]);
            const std::string action = str(step["action"]);
            harness.device.beginStep(step);
            if (action == "boot")
            {
                NivaloImageIdentity image;
                const bool hasImage = !step["runningImage"].isNull();
                if (hasImage)
                {
                    image = imageFrom(step["runningImage"], harness.device);
                }
                else
                {
                    harness.device.hasSlot = false;
                }
                harness.device.interrupt = str(step["interrupt"]);
                harness.boot(hasImage ? &image : NULL, false);
                if (harness.powered)
                {
                    // ACKs the device publishes after its next birth.
                    harness.executor->republishResult();
                }
            }
            else
            {
                V11_CHECK(harness.powered, "a power-loss step must be followed by boot");
                try
                {
                    if (action == "deliver")
                    {
                        harness.device.stage = harness.executor->busy() ? harness.device.stage : STAGE_IDLE;
                        harness.deliver(step);
                    }
                    else if (action == "complete")
                    {
                        harness.complete();
                    }
                    else if (action == "heartbeat")
                    {
                        harness.executor->republishResult();
                    }
                    else
                    {
                        V11_CHECK(false, "unknown action " << action);
                    }
                }
                catch (const PowerCut &)
                {
                    harness.powered = false;
                }
            }
            checkExpectations(harness, step["expect"].as<JsonObjectConst>());
            checked++;
        }
    }
    return checked;
}

int clockValidityCases(JsonObjectConst root)
{
    int checked = 0;
    for (JsonObjectConst sample : root["clockValidity"].as<JsonArrayConst>())
    {
        context() = std::string("clockValidity: ") + str(sample["description"]);
        NivaloClock clock;
        const int64_t base = 1000000;
        const int64_t utcBase = 1795000000000LL;
        int64_t now = base;
        if (sample["syncedThisBoot"].as<bool>())
        {
            clock.onSync(utcBase, base);
            const double step = sample["backwardStepSeconds"].as<double>();
            if (step > 0)
            {
                // A second synchronisation 10 s later that steps back by step seconds.
                const int64_t later = base + 10000000;
                clock.onSync(utcBase + 10000 - (int64_t)std::llround(step * 1000.0), later);
                now = later;
            }
        }
        now += (int64_t)std::llround(sample["secondsSinceSync"].as<double>() * 1000000.0);
        V11_CHECK(clock.valid(now) == sample["valid"].as<bool>(), "valid " << clock.valid(now));
        checked++;
    }
    return checked;
}

int fingerprintCases(JsonObjectConst root)
{
    int checked = 0;
    NivaloTopics topics;
    topics.configure(DeviceId);
    for (JsonObjectConst sample : root["fingerprints"].as<JsonArrayConst>())
    {
        context() = std::string("fingerprint: ") + str(sample["description"]);
        const std::string envelope = str(sample["envelope"]);
        DynamicJsonDocument parsed(8192);
        deserializeJson(parsed, envelope);
        const std::string topic = std::string("nivalo/v1.1/devices/") + DeviceId + "/commands/" + str(parsed["correlationId"]);
        NivaloOperation operation;
        const NivaloDownlinkOutcome outcome = nivaloParseDownlink(
            topics, topic.c_str(), reinterpret_cast<const uint8_t *>(envelope.data()), envelope.size(), &operation);
        char hex[65];
        nivaloHexEncode(operation.fingerprint, 32U, hex);
        V11_CHECK(outcome == NIVALO_DOWNLINK_IDENTIFIED && std::string(hex) == str(sample["sha256"]),
                  "fingerprint " << hex);
        checked++;
    }
    return checked;
}
} // namespace

int main()
{
    std::unique_ptr<DynamicJsonDocument> document = loadFixture("fixtures/v1.1/vectors/command-execution.json");
    JsonObjectConst root = document->as<JsonObjectConst>();
    int checked = clockValidityCases(root);
    checked += fingerprintCases(root);
    checked += runCases(root);
    return finish("command-execution vectors", checked);
}
