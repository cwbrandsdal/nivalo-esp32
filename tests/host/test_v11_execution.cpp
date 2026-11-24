// Replays nivalo-protocol fixtures/v1.1/vectors/command-execution.json against the firmware's
// downlink parser, executor and journal (specs/cloud-mqtt-v1.1.md sections 9, 10.4 and 11.4).
// Power loss is simulated by an exception thrown from the fake storage; the durable state of the
// fake survives it and the next boot step builds a new executor from that state, as a reset would.

#include "NivaloArguments.h"
#include "NivaloDownlink.h"
#include "NivaloExecutor.h"
#include "NivaloJournal.h"
#include "NivaloOtaPolicy.h"
#include "v11_test_support.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
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
    // Platform calls in order ("prepare", "clock", "claim", "done", "invoke", ...).
    std::vector<std::string> calls;
    // The invocation token handed to the last handler or download.
    uint32_t lastToken = 0U;
    // The arguments object the validator and the handler received, as prepared once.
    NivaloPreparedArguments prepared;
    // Focused cases: a handler and validator of their own instead of the fixture's schema.
    std::function<bool(JsonObjectConst)> validator;
    std::function<NivaloFunctionOutcome(JsonObjectConst)> customHandler;
    // Storage failures that return false (not a reset).
    bool failTerminalWrite = false;
    bool powerCutOnClearPending = false;

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
        calls.push_back(claim ? "claim" : "done");
        if (!claim && !booting && failTerminalWrite)
        {
            return false;
        }
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
        calls.push_back("clearPending");
        if (powerCutOnClearPending)
        {
            throw PowerCut();
        }
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
        calls.push_back("clock");
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

    bool hasFunction(const char *name) override { return customHandler ? true : functions.containsKey(name); }

    // The production preparation: the original argument bytes, parsed once.
    bool prepareFunction(const NivaloOperation &operation) override
    {
        calls.push_back("prepare");
        if (!prepared.prepare(operation.arguments, operation.argumentsLength))
        {
            return false;
        }
        const bool accepted = validator ? validator(prepared.object())
                                        : schemaAccepts(functions[operation.command], prepared.object());
        if (!accepted)
        {
            prepared.release();
        }
        return accepted;
    }

    NivaloFunctionOutcome invokePrepared(uint32_t token) override
    {
        calls.push_back("invoke");
        handlerCalls++;
        lastToken = token;
        V11_CHECK(prepared.ready(), "the handler runs only with prepared arguments");
        return customHandler ? customHandler(prepared.object()) : outcome(handler);
    }

    void releasePrepared() override { prepared.release(); }

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

    NivaloRefreshOutcome refreshCatalog() override
    {
        refreshes++;
        return NIVALO_REFRESH_COMPLETE;
    }

    NivaloDownloadOutcome downloadImage(const NivaloOperation &, uint32_t token) override
    {
        lastToken = token;
        return download(handler);
    }

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
            V11_CHECK(executor->completeDownload(device.lastToken,
                                                 device.download(device.handler.empty() ? "installed" : device.handler)),
                      "completion of the running download refused");
        }
        else
        {
            V11_CHECK(executor->completeFunction(device.lastToken, device.outcome(device.handler)),
                      "completion of the running handler refused");
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

// --- Focused regressions -------------------------------------------------------------------------

const char FirmwareMember[] = ",\"firmware\":{\"version\":\"1.0.0\",\"hardware\":\"esp32-standalone\",\"targets\":[\"esp32\"]}";

std::string commandIdFor(uint32_t seq)
{
    char id[37];
    std::snprintf(id, sizeof(id), "44444444-4444-4444-8444-4444%08lx", (unsigned long)seq);
    return id;
}

// One identified downlink through the real parser; extra is spliced into the envelope.
NivaloOperation parsed(const std::string &route, const std::string &id, const std::string &payload,
                       const std::string &extra = "")
{
    std::string envelope = envelopeFor(id, payload);
    envelope.insert(envelope.size() - 1U, extra);
    const std::string topic = std::string("nivalo/v1.1/devices/") + DeviceId + "/" + route + "/" + id;
    NivaloTopics topics;
    topics.configure(DeviceId);
    NivaloOperation operation;
    V11_CHECK(nivaloParseDownlink(topics, topic.c_str(), reinterpret_cast<const uint8_t *>(envelope.data()),
                                  envelope.size(), &operation) == NIVALO_DOWNLINK_IDENTIFIED,
              "downlink not identified");
    return operation;
}

NivaloOperation commandOperation(uint32_t seq, const std::string &arguments, const std::string &extra = "")
{
    const std::string payload = "{\"commandSeq\":" + std::to_string(seq) +
                                ",\"issuedAt\":\"2026-11-24T12:00:00Z\",\"expiresAt\":\"2026-11-24T12:05:00Z\","
                                "\"command\":\"configure\",\"arguments\":" + arguments + "}";
    return parsed("commands", commandIdFor(seq), payload, extra);
}

// A device already booted with an empty journal, its clock at 12:00:30.
struct Focus
{
    DynamicJsonDocument caseDocument{256};
    DynamicJsonDocument stepDocument{1024};
    std::unique_ptr<Harness> harness;

    explicit Focus(JsonObjectConst root)
    {
        deserializeJson(caseDocument, "{\"journal\":{\"highWater\":0}}");
        harness.reset(new Harness(root, caseDocument.as<JsonObjectConst>()));
        step("{\"clock\":{\"valid\":true,\"now\":\"2026-11-24T12:00:30Z\"}}");
    }
    void step(const char *json)
    {
        stepDocument.clear();
        deserializeJson(stepDocument, json);
        device().beginStep(stepDocument.as<JsonVariantConst>());
        device().calls.clear();
    }
    FakeDevice &device() { return harness->device; }
    NivaloExecutor &executor() { return *harness->executor; }
    std::string acks() const
    {
        std::string text;
        for (const NivaloAck &ack : harness->device.acks) text += ackText(ack);
        return text;
    }
};

size_t indexOf(const std::vector<std::string> &calls, const std::string &name)
{
    for (size_t i = 0; i < calls.size(); i++)
        if (calls[i] == name) return i;
    return calls.size();
}

int asyncCompletionCases(JsonObjectConst root)
{
    int checked = 0;
    Focus focus(root);
    focus.device().validator = [](JsonObjectConst) { return true; };
    focus.device().customHandler = [](JsonObjectConst) { return NIVALO_FUNCTION_PENDING; };

    context() = "B2: a stale or unknown token completes nothing";
    focus.executor().deliver(commandOperation(43U, "{\"to\":1}"));
    const uint32_t first = focus.device().lastToken;
    V11_CHECK(first != 0U && focus.executor().busy() && focus.acks() == "{43 accepted}", focus.acks());
    focus.step("{}");
    V11_CHECK(!focus.executor().completeFunction(first + 1U, NIVALO_FUNCTION_SUCCEEDED), "unknown token accepted");
    V11_CHECK(!focus.executor().completeFunction(0U, NIVALO_FUNCTION_SUCCEEDED), "zero token accepted");
    V11_CHECK(!focus.executor().completeFunction(first, NIVALO_FUNCTION_PENDING), "pending is not a completion");
    V11_CHECK(focus.device().acks.empty() && focus.device().journal.phase == NIVALO_PHASE_CLAIMED &&
                  focus.executor().busy(),
              "a rejected completion changed the journal");
    V11_CHECK(focus.executor().completeFunction(first, NIVALO_FUNCTION_SUCCEEDED) && !focus.executor().busy() &&
                  focus.acks() == "{43 succeeded}",
              focus.acks());
    checked++;

    context() = "B2: a late duplicate completion of A cannot finish B";
    focus.step("{\"clock\":{\"valid\":true,\"now\":\"2026-11-24T12:00:30Z\"}}");
    focus.executor().deliver(commandOperation(44U, "{\"to\":2}"));
    const uint32_t second = focus.device().lastToken;
    V11_CHECK(second != first && focus.executor().busy(), "second operation not running");
    focus.step("{}");
    V11_CHECK(!focus.executor().completeFunction(first, NIVALO_FUNCTION_FAILED), "A's token completed B");
    V11_CHECK(focus.device().acks.empty() && focus.device().journal.seq == 44U &&
                  focus.device().journal.phase == NIVALO_PHASE_CLAIMED,
              "B's claim was overwritten");
    V11_CHECK(focus.executor().completeFunction(second, NIVALO_FUNCTION_SUCCEEDED) && focus.acks() == "{44 succeeded}",
              focus.acks());
    checked++;

    context() = "B2: busy ends only once the terminal result is durable";
    focus.step("{\"clock\":{\"valid\":true,\"now\":\"2026-11-24T12:00:30Z\"}}");
    focus.executor().deliver(commandOperation(45U, "{\"to\":3}"));
    const uint32_t third = focus.device().lastToken;
    focus.step("{\"clock\":{\"valid\":true,\"now\":\"2026-11-24T12:00:30Z\"}}");
    focus.device().failTerminalWrite = true;
    V11_CHECK(!focus.executor().completeFunction(third, NIVALO_FUNCTION_SUCCEEDED), "undurable completion accepted");
    V11_CHECK(focus.executor().busy() && focus.executor().blocked() && focus.device().acks.empty() &&
                  focus.device().journal.phase == NIVALO_PHASE_CLAIMED,
              "busy released or result ACKed before it was durable");
    focus.executor().deliver(commandOperation(46U, "{\"to\":4}"));
    V11_CHECK(focus.device().handlerCalls == 0 && focus.device().acks.empty(), "a new operation started");
    checked++;
    return checked;
}

int argumentCases(JsonObjectConst root)
{
    int checked = 0;
    context() = "H3: a value just below a threshold reaches validator and handler unchanged";
    {
        Focus focus(root);
        double validated = -1.0, handled = -1.0;
        focus.device().validator = [&validated](JsonObjectConst arguments) {
            validated = arguments["setpoint"].as<double>();
            return arguments["setpoint"].is<double>() && validated < 1.0;
        };
        focus.device().customHandler = [&handled](JsonObjectConst arguments) {
            handled = arguments["setpoint"].as<double>();
            return NIVALO_FUNCTION_SUCCEEDED;
        };
        const NivaloOperation operation = commandOperation(43U, "{\"setpoint\":0.9999999999}");
        V11_CHECK(std::string(operation.arguments) == "{\"setpoint\":0.9999999999}", "argument bytes changed");
        focus.executor().deliver(operation);
        const double sent = std::strtod("0.9999999999", NULL);
        V11_CHECK(focus.acks() == "{43 accepted}{43 succeeded}", focus.acks());
        V11_CHECK(validated == sent && handled == sent && handled < 1.0,
                  "validator saw " << validated << ", handler saw " << handled);
        checked++;
    }

    context() = "H3: a non-finite value is refused before the claim";
    {
        Focus focus(root);
        focus.device().validator = [](JsonObjectConst) { return true; };
        focus.device().customHandler = [](JsonObjectConst) { return NIVALO_FUNCTION_SUCCEEDED; };
        focus.executor().deliver(commandOperation(43U, "{\"setpoint\":1e999}"));
        V11_CHECK(focus.acks() == "{43 failed invalid_command}" && focus.device().handlerCalls == 0 &&
                      indexOf(focus.device().calls, "claim") == focus.device().calls.size(),
                  focus.acks());
        checked++;
    }

    context() = "H3: unrepresentable numbers";
    {
        NivaloPreparedArguments arguments;
        const char *const refused[] = {"{\"v\":1e999}", "{\"v\":-1e999}", "{\"v\":1e-999}", "{\"v\":[1,2e400]}",
                                       "{\"v\":99999999999999999999}", "{\"v\":-9223372036854775809}"};
        for (const char *text : refused)
            V11_CHECK(!arguments.prepare(text, std::strlen(text)), "accepted " << text);
        const char *const accepted[] = {"{\"v\":0}", "{\"v\":-0.0}", "{\"v\":18446744073709551615}",
                                        "{\"v\":-9223372036854775808}", "{\"v\":{\"w\":[1.5,\"x\",true]}}", "{}"};
        for (const char *text : accepted)
            V11_CHECK(arguments.prepare(text, std::strlen(text)), "refused " << text);
        checked++;
    }
    return checked;
}

int orderingCases(JsonObjectConst root)
{
    int checked = 0;
    context() = "H7: the handler call directly follows the final clock reading";
    Focus focus(root);
    focus.device().validator = [](JsonObjectConst) { return true; };
    focus.device().customHandler = [](JsonObjectConst) { return NIVALO_FUNCTION_SUCCEEDED; };
    focus.executor().deliver(commandOperation(43U, "{\"to\":1}"));
    const std::vector<std::string> &calls = focus.device().calls;
    const size_t prepare = indexOf(calls, "prepare"), claim = indexOf(calls, "claim"), invoke = indexOf(calls, "invoke");
    std::string trace;
    for (const std::string &call : calls) trace += call + " ";
    V11_CHECK(prepare < claim && claim < invoke && invoke < calls.size(), trace);
    V11_CHECK(invoke > 0U && calls[invoke - 1U] == "clock", "work between the final reading and the handler: " << trace);
    checked++;
    return checked;
}

// The signed OTA job of the vectors (accepted by the default device).
std::string otaPayload(JsonObjectConst root, std::string *id)
{
    for (JsonObjectConst testCase : root["cases"].as<JsonArrayConst>())
        for (JsonObjectConst step : testCase["steps"].as<JsonArrayConst>())
            if (std::strcmp(str(step["route"]), "ota") == 0 && std::strcmp(str(step["handler"]), "downloading") == 0)
            {
                *id = str(step["commandId"]);
                return compact(step["payload"]);
            }
    V11_CHECK(false, "no OTA job in the vectors");
    return "";
}

int pendingCleanupCases(JsonObjectConst root)
{
    int checked = 0;
    std::string id;
    const std::string payload = otaPayload(root, &id);
    // Downloaded and pending, then the activation reading is past expiresAt.
    const char ExpiredAfterPending[] = "{\"handler\":\"installed\",\"clock\":{\"valid\":true,\"now\":\"2026-11-24T12:00:30Z\","
                                       "\"start\":{\"valid\":true,\"now\":\"2026-11-24T14:00:01Z\",\"elapsed\":7171}}}";

    context() = "M11: a failed terminal write keeps the pending record";
    {
        Focus focus(root);
        focus.step(ExpiredAfterPending);
        focus.device().failTerminalWrite = true;
        focus.executor().deliver(parsed("ota", id, payload));
        V11_CHECK(focus.device().pendingStored && focus.device().journal.phase == NIVALO_PHASE_CLAIMED &&
                      focus.device().activations == 0 && focus.acks() == "{43 accepted}",
                  "pending discarded before the result was durable: " << focus.acks());
        focus.device().failTerminalWrite = false;
        focus.harness->boot(NULL, false);
        V11_CHECK(focus.device().journal.phase == NIVALO_PHASE_DONE && !focus.device().pendingStored &&
                      resultText(focus.device().journal.result) == "indeterminate/execution_unknown",
                  "boot did not reconcile with the kept evidence: " << resultText(focus.device().journal.result));
        checked++;
    }

    context() = "M11: an activation refusal whose result cannot be stored keeps the pending record";
    {
        Focus focus(root);
        focus.step("{\"handler\":\"activationRejected\",\"clock\":{\"valid\":true,\"now\":\"2026-11-24T12:00:30Z\","
                   "\"start\":{\"valid\":true,\"now\":\"2026-11-24T12:10:00Z\",\"elapsed\":570}}}");
        focus.device().failTerminalWrite = true;
        focus.executor().deliver(parsed("ota", id, payload));
        V11_CHECK(focus.device().activations == 1 && focus.device().pendingStored &&
                      focus.device().journal.phase == NIVALO_PHASE_CLAIMED,
                  "pending discarded before the result was durable");
        checked++;
    }

    context() = "M11: a reset after the durable result only leaves a record to clear";
    {
        Focus focus(root);
        focus.step(ExpiredAfterPending);
        focus.device().powerCutOnClearPending = true;
        try
        {
            focus.executor().deliver(parsed("ota", id, payload));
            V11_CHECK(false, "the clear was not reached");
        }
        catch (const PowerCut &)
        {
        }
        V11_CHECK(focus.device().pendingStored && resultText(focus.device().journal.result) == "failed/expired" &&
                      focus.acks() == "{43 accepted}{43 failed expired}",
                  focus.acks());
        focus.device().powerCutOnClearPending = false;
        focus.harness->boot(NULL, false);
        V11_CHECK(!focus.device().pendingStored && resultText(focus.device().journal.result) == "failed/expired",
                  "boot changed a durable result");
        checked++;
    }
    return checked;
}

int firmwarePlacementCases(JsonObjectConst root)
{
    int checked = 0;
    context() = "M1: firmware metadata on a command is invalid, with no effect";
    {
        Focus focus(root);
        focus.device().validator = [](JsonObjectConst) { return true; };
        focus.device().customHandler = [](JsonObjectConst) { return NIVALO_FUNCTION_SUCCEEDED; };
        const NivaloOperation operation = commandOperation(43U, "{\"to\":1}", FirmwareMember);
        V11_CHECK(!operation.valid, "firmware placement accepted");
        focus.executor().deliver(operation);
        V11_CHECK(focus.acks() == "{43 failed invalid_command}" && focus.device().handlerCalls == 0, focus.acks());
        checked++;
    }
    context() = "M1: firmware metadata on an OTA job is invalid, with no effect";
    {
        Focus focus(root);
        focus.step("{\"handler\":\"installed\",\"clock\":{\"valid\":true,\"now\":\"2026-11-24T12:00:30Z\"}}");
        std::string id;
        const std::string payload = otaPayload(root, &id);
        const NivaloOperation operation = parsed("ota", id, payload, FirmwareMember);
        V11_CHECK(!operation.valid, "firmware placement accepted");
        focus.executor().deliver(operation);
        V11_CHECK(focus.acks() == "{43 failed invalid_command}" && focus.device().activations == 0 &&
                      !focus.device().pendingStored,
                  focus.acks());
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
    checked += asyncCompletionCases(root);
    checked += argumentCases(root);
    checked += orderingCases(root);
    checked += pendingCleanupCases(root);
    checked += firmwarePlacementCases(root);
    return finish("command-execution vectors", checked);
}
