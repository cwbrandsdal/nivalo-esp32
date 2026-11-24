// Capability catalog (specs/cloud-mqtt-v1.1.md section 12).
//
// The server merge is not firmware, but the firmware's refresh must merge cleanly. This test
// ports the reference merge (tests/protocol_v1_1.py Catalog), proves the port against
// fixtures/v1.1/vectors/capability-merge.json, and then feeds it the packets the firmware's
// catalog planner produces across releases: first report, unchanged re-report, changed
// definition, removal, case-only rename and a full-size catalog.

#include "NivaloCatalog.h"
#include "NivaloJsonStrict.h"
#include "NivaloUplink.h"
#include "v11_test_support.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace v11test;

namespace
{
typedef std::pair<std::string, std::string> Key; // (kind, name)
typedef std::map<std::string, std::string> Definition; // member -> compact value

std::string lower(std::string value)
{
    for (char &c : value)
    {
        c = (char)std::tolower((unsigned char)c);
    }
    return value;
}

struct Entry
{
    long revision = 0;
    bool live = false;
    Definition definition;
};

Definition normalized(const std::string &kind, JsonObjectConst definition)
{
    Definition result;
    if (kind == "variable")
    {
        result["historyEnabled"] = "true";
        result["sortOrder"] = "0";
    }
    else
    {
        result["timeoutSeconds"] = "30";
        result["dangerLevel"] = "\"safe\"";
        result["sortOrder"] = "0";
    }
    for (JsonPairConst member : definition)
    {
        result[member.key().c_str()] = compact(member.value());
    }
    return result;
}

bool textOk(JsonVariantConst value, size_t max) { return nivaloIsText(str(value, NULL), max); }

// Enough of capability-operations.schema.json for the vectors and the firmware's packets.
bool packetSchemaValid(JsonObjectConst packet)
{
    static const std::set<std::string> Top = {"source", "revision", "ops"};
    for (JsonPairConst member : packet)
    {
        if (!Top.count(member.key().c_str())) return false;
    }
    if (std::strcmp(str(packet["source"]), "esp32") != 0 || !packet["revision"].is<long long>() ||
        packet["revision"].as<long long>() < 1 || packet["revision"].as<long long>() > 2147483647LL ||
        !packet["ops"].is<JsonArrayConst>())
    {
        return false;
    }
    JsonArrayConst ops = packet["ops"].as<JsonArrayConst>();
    if (ops.size() < 1 || ops.size() > 8) return false;
    for (JsonObjectConst op : ops)
    {
        const std::string kind = str(op["kind"]), action = str(op["op"]);
        if ((kind != "variable" && kind != "function") || (action != "upsert" && action != "remove")) return false;
        const char *name = str(op["name"], NULL);
        if (!(kind == "function" ? nivaloIsFunctionName(name) : nivaloIsName(name))) return false;
        const size_t expected = action == "upsert" ? 4U : 3U;
        if (op.size() != expected || (action == "upsert" && !op["definition"].is<JsonObjectConst>())) return false;
        if (action == "remove") continue;
        JsonObjectConst definition = op["definition"].as<JsonObjectConst>();
        for (JsonPairConst member : definition)
        {
            const std::string field = member.key().c_str();
            JsonVariantConst value = member.value();
            bool ok;
            if (field == "displayName") ok = textOk(value, 128);
            else if (field == "description") ok = textOk(value, 256);
            else if (field == "sortOrder") ok = value.is<long long>() && value.as<long long>() >= 0 && value.as<long long>() <= 32767;
            else if (kind == "variable" && field == "dataType")
                ok = std::string(str(value)) == "number" || std::string(str(value)) == "boolean" || std::string(str(value)) == "string";
            else if (kind == "variable" && field == "unit") ok = textOk(value, 32);
            else if (kind == "variable" && field == "historyEnabled") ok = value.is<bool>();
            else if (kind == "function" && field == "argumentExample")
                ok = value.is<const char *>() && std::strlen(str(value)) >= 1 && std::strlen(str(value)) <= 256;
            else if (kind == "function" && field == "returnType")
            {
                static const std::set<std::string> Types = {"integer", "number", "string", "boolean", "object", "void"};
                ok = Types.count(str(value)) > 0;
            }
            else if (kind == "function" && field == "timeoutSeconds")
                ok = value.is<long long>() && value.as<long long>() >= 1 && value.as<long long>() <= 600;
            else if (kind == "function" && field == "dangerLevel")
            {
                static const std::set<std::string> Levels = {"safe", "operational", "disruptive", "dangerous"};
                ok = Levels.count(str(value)) > 0;
            }
            else ok = false;
            if (!ok) return false;
        }
        if (kind == "variable" && !definition.containsKey("dataType")) return false;
    }
    return true;
}

bool duplicateCapability(JsonArrayConst ops)
{
    std::set<Key> keys, upserts;
    for (JsonObjectConst op : ops)
    {
        if (!keys.insert(Key(str(op["kind"]), str(op["name"]))).second) return true;
        if (std::strcmp(str(op["op"]), "upsert") == 0 && !upserts.insert(Key(str(op["kind"]), lower(str(op["name"])))).second)
            return true;
    }
    return false;
}

// tests/protocol_v1_1.py Catalog.apply
class ServerCatalog
{
public:
    std::map<Key, Entry> entries;

    std::string apply(JsonObjectConst packet)
    {
        if (!packetSchemaValid(packet)) return "schema";
        JsonArrayConst ops = packet["ops"].as<JsonArrayConst>();
        if (duplicateCapability(ops)) return "duplicate-capability";
        const long revision = packet["revision"].as<long>();
        std::map<Key, std::pair<bool, Entry>> changes; // key -> (applies, entry)
        for (JsonObjectConst op : ops)
        {
            const Key key(str(op["kind"]), str(op["name"]));
            Entry change;
            change.revision = revision;
            change.live = std::strcmp(str(op["op"]), "upsert") == 0;
            if (change.live) change.definition = normalized(key.first, op["definition"].as<JsonObjectConst>());
            std::map<Key, Entry>::const_iterator current = entries.find(key);
            if (current != entries.end() && revision == current->second.revision &&
                (current->second.live != change.live || current->second.definition != change.definition))
            {
                return "revision-conflict";
            }
            changes[key] = std::make_pair(current == entries.end() || revision > current->second.revision, change);
        }
        std::map<Key, std::vector<Key>> groups;
        std::map<Key, bool> groupHasUpsert;
        for (JsonObjectConst op : ops)
        {
            const Key group(str(op["kind"]), lower(str(op["name"])));
            groups[group].push_back(Key(str(op["kind"]), str(op["name"])));
            if (std::strcmp(str(op["op"]), "upsert") == 0) groupHasUpsert[group] = true;
        }
        for (const auto &group : groups)
        {
            if (!groupHasUpsert[group.first] || group.second.size() < 2) continue;
            bool skipped = false;
            for (const Key &key : group.second) skipped = skipped || !changes[key].first;
            if (skipped)
                for (const Key &key : group.second) changes[key].first = false;
        }
        std::map<Key, Entry> staged = entries;
        for (const auto &change : changes)
            if (change.second.first) staged[change.first] = change.second.second;
        std::set<Key> liveFolded;
        size_t live = 0;
        for (const auto &entry : staged)
        {
            if (!entry.second.live) continue;
            live++;
            if (!liveFolded.insert(Key(entry.first.first, lower(entry.first.second))).second) return "name-collision";
        }
        if (live > 256 || staged.size() > 512) return "limit";
        entries = staged;
        return "";
    }

    size_t liveCount() const
    {
        size_t count = 0;
        for (const auto &entry : entries) count += entry.second.live ? 1 : 0;
        return count;
    }

    std::string snapshot() const
    {
        std::string text;
        for (const auto &entry : entries)
            text += entry.first.first + ":" + entry.first.second + "@" + std::to_string(entry.second.revision) +
                    (entry.second.live ? " live; " : " deleted; ");
        return text;
    }
};

void seed(ServerCatalog &catalog, JsonObjectConst initial)
{
    std::vector<std::pair<Key, Entry>> seeds;
    auto add = [&](const std::string &kind, const std::string &name, long revision, JsonVariantConst definition) {
        Entry entry;
        entry.revision = revision;
        entry.live = !definition.isNull();
        if (entry.live) entry.definition = normalized(kind, definition.as<JsonObjectConst>());
        catalog.entries[Key(kind, name)] = entry;
    };
    for (JsonObjectConst entry : initial["entries"].as<JsonArrayConst>())
        add(str(entry["kind"]), str(entry["name"]), entry["revision"].as<long>(), entry["definition"]);
    for (JsonObjectConst range : initial["generated"].as<JsonArrayConst>())
    {
        for (int i = 0; i < range["count"].as<int>(); i++)
        {
            char name[80];
            std::snprintf(name, sizeof(name), "%s%03d", str(range["namePrefix"]), i);
            add(str(range["kind"]), name, range["revision"].as<long>(), range["definition"]);
        }
    }
}

int mergeVectorCases()
{
    std::unique_ptr<DynamicJsonDocument> document = loadFixture("fixtures/v1.1/vectors/capability-merge.json");
    int checked = 0;
    for (JsonObjectConst testCase : (*document)["cases"].as<JsonArrayConst>())
    {
        ServerCatalog catalog;
        seed(catalog, testCase["initial"].as<JsonObjectConst>());
        int index = 0;
        for (JsonObjectConst step : testCase["steps"].as<JsonArrayConst>())
        {
            context() = std::string("capability-merge ") + str(testCase["id"]) + " step " + std::to_string(index++);
            JsonObjectConst expect = step["expect"].as<JsonObjectConst>();
            const std::string rule = catalog.apply(step["packet"].as<JsonObjectConst>());
            const bool accepted = std::strcmp(str(expect["outcome"]), "accepted") == 0;
            V11_CHECK(rule.empty() == accepted && (accepted || rule == str(expect["rule"])),
                      "rule '" << rule << "' expected " << str(expect["outcome"]) << " " << str(expect["rule"]));
            if (expect.containsKey("catalog"))
            {
                std::string expected;
                for (JsonObjectConst entry : expect["catalog"].as<JsonArrayConst>())
                    expected += std::string(str(entry["kind"])) + ":" + str(entry["name"]) + "@" +
                                std::to_string(entry["revision"].as<long>()) +
                                (std::strcmp(str(entry["state"]), "live") == 0 ? " live; " : " deleted; ");
                V11_CHECK(catalog.snapshot() == expected, catalog.snapshot() << " expected " << expected);
            }
            if (expect.containsKey("liveCount"))
            {
                V11_CHECK(catalog.liveCount() == expect["liveCount"].as<size_t>() &&
                              catalog.entries.size() == expect["totalCount"].as<size_t>(),
                          "counts " << catalog.liveCount() << "/" << catalog.entries.size());
            }
            checked++;
        }
    }
    return checked;
}

// --- The firmware's planner ---------------------------------------------------------------------

struct Capability
{
    NivaloCapabilityKind kind;
    std::string name;
    std::string definition;
};

class Source : public NivaloCatalogSource
{
public:
    std::vector<Capability> capabilities;
    size_t capabilityCount() const override { return capabilities.size(); }
    NivaloCapabilityKind capabilityKind(size_t index) const override { return capabilities[index].kind; }
    const char *capabilityName(size_t index) const override { return capabilities[index].name.c_str(); }
    void writeDefinition(size_t index, NivaloJsonWriter &writer) const override
    {
        writer.raw(capabilities[index].definition.c_str(), capabilities[index].definition.size());
    }
};

class Collector : public NivaloCatalogSink
{
public:
    std::vector<std::string> packets;
    bool emitPacket(const char *payload, size_t length) override
    {
        packets.push_back(std::string(payload, length));
        return true;
    }
};

struct Release
{
    const char *description;
    Source source;
    NivaloCatalogDecision decision;
    uint32_t revision;
};

// Plans, persists (through the blob codec, as the device does), refreshes and merges one release.
void report(Source &source, std::unique_ptr<NivaloCatalogState> &stored, ServerCatalog &server,
            NivaloCatalogDecision expectedDecision, uint32_t expectedRevision, size_t *packetCount)
{
    NivaloCatalogState next;
    const NivaloCatalogDecision decision = nivaloPlanCatalog(source, stored.get(), &next);
    V11_CHECK(decision == expectedDecision, "decision " << decision);
    V11_CHECK(next.revision == expectedRevision, "revision " << next.revision << " expected " << expectedRevision);
    std::vector<uint8_t> blob(NivaloCatalogBlobBytes);
    const size_t length = nivaloEncodeCatalog(next, blob.data(), blob.size());
    NivaloCatalogState decoded;
    V11_CHECK(length > 0 && nivaloDecodeCatalog(blob.data(), length, &decoded) && decoded.revision == next.revision &&
                  decoded.liveCount == next.liveCount && decoded.retiredCount == next.retiredCount,
              "catalog blob round trip");
    stored.reset(new NivaloCatalogState(decoded));

    Collector collector;
    V11_CHECK(nivaloRefreshCatalog(source, decoded, NivaloTelemetryBatch::PayloadCapacity, collector), "refresh");
    for (const std::string &packet : collector.packets)
    {
        V11_CHECK(nivaloScanJson(packet.data(), packet.size(), NivaloV11::MaxDepth - 1U, NivaloV11::MaxValues, NULL) ==
                      NIVALO_JSON_OK,
                  "packet is strict JSON");
        char envelope[NivaloV11::MaxUplinkBytes + 1];
        V11_CHECK(nivaloBuildEnvelope("22222222-2222-4222-8222-222222222222", "33333333-3333-4333-8333-333300000001",
                                      1795521605000LL, NULL, NULL, packet.data(), packet.size(), envelope,
                                      sizeof(envelope)) > 0,
                  "packet fits an envelope");
        DynamicJsonDocument document(16384);
        deserializeJson(document, packet);
        V11_CHECK(document["ops"].as<JsonArrayConst>().size() <= 8U, "at most 8 operations");
        const std::string rule = server.apply(document.as<JsonObjectConst>());
        V11_CHECK(rule.empty(), "server rejected the firmware's packet: " << rule << " " << packet);
    }
    *packetCount = collector.packets.size();
}

std::string expectLive(const ServerCatalog &server, const std::string &kind, const std::string &name, long revision)
{
    std::map<Key, Entry>::const_iterator found = server.entries.find(Key(kind, name));
    if (found == server.entries.end()) return "missing";
    if (!found->second.live) return "deleted";
    return found->second.revision == revision ? "" : "revision " + std::to_string(found->second.revision);
}

int plannerCases()
{
    int checked = 0;
    ServerCatalog server;
    std::unique_ptr<NivaloCatalogState> stored;
    size_t packets = 0;
    Source source;
    source.capabilities = {
        {NIVALO_KIND_VARIABLE, "temp", "{\"dataType\":\"number\",\"unit\":\"C\",\"historyEnabled\":true,\"sortOrder\":0}"},
        {NIVALO_KIND_VARIABLE, "humidity", "{\"dataType\":\"number\",\"unit\":\"%\",\"historyEnabled\":true,\"sortOrder\":1}"},
        {NIVALO_KIND_FUNCTION, "setLed",
         "{\"argumentExample\":\"{\\\"state\\\":true}\",\"returnType\":\"void\",\"timeoutSeconds\":30,\"dangerLevel\":\"operational\",\"sortOrder\":0}"},
    };

    context() = "first report";
    report(source, stored, server, NIVALO_CATALOG_CHANGED, 1U, &packets);
    V11_CHECK(expectLive(server, "variable", "temp", 1).empty() && expectLive(server, "function", "setLed", 1).empty(),
              server.snapshot());
    checked++;

    context() = "unchanged re-report reuses the revision";
    const std::string before = server.snapshot();
    report(source, stored, server, NIVALO_CATALOG_UNCHANGED, 1U, &packets);
    V11_CHECK(server.snapshot() == before, server.snapshot());
    checked++;

    context() = "changed definition advances the revision";
    source.capabilities[0].definition = "{\"dataType\":\"number\",\"unit\":\"K\",\"historyEnabled\":true,\"sortOrder\":0}";
    report(source, stored, server, NIVALO_CATALOG_CHANGED, 2U, &packets);
    V11_CHECK(expectLive(server, "variable", "temp", 2).empty() && expectLive(server, "variable", "humidity", 2).empty(),
              server.snapshot());
    checked++;

    context() = "removal leaves a tombstone";
    source.capabilities.erase(source.capabilities.begin() + 1);
    report(source, stored, server, NIVALO_CATALOG_CHANGED, 3U, &packets);
    V11_CHECK(expectLive(server, "variable", "humidity", 3) == "deleted", server.snapshot());
    checked++;

    context() = "case-only rename";
    source.capabilities[0].name = "Temp";
    NivaloCatalogState planned;
    nivaloPlanCatalog(source, stored.get(), &planned);
    Collector pair;
    nivaloRefreshCatalog(source, planned, NivaloTelemetryBatch::PayloadCapacity, pair);
    bool together = false;
    for (const std::string &packet : pair.packets)
        together = together || (packet.find("\"name\":\"temp\"") != std::string::npos &&
                                packet.find("\"name\":\"Temp\"") != std::string::npos);
    V11_CHECK(together, "the rename's remove and upsert share a packet");
    report(source, stored, server, NIVALO_CATALOG_CHANGED, 4U, &packets);
    V11_CHECK(expectLive(server, "variable", "Temp", 4).empty() && expectLive(server, "variable", "temp", 4) == "deleted",
              server.snapshot());
    checked++;

    context() = "renaming back revives the old spelling";
    source.capabilities[0].name = "temp";
    report(source, stored, server, NIVALO_CATALOG_CHANGED, 5U, &packets);
    V11_CHECK(expectLive(server, "variable", "temp", 5).empty() && expectLive(server, "variable", "Temp", 5) == "deleted" &&
                  expectLive(server, "variable", "humidity", 5) == "deleted",
              server.snapshot());
    checked++;

    context() = "a reboot with the same firmware changes nothing";
    const std::string settled = server.snapshot();
    report(source, stored, server, NIVALO_CATALOG_UNCHANGED, 5U, &packets);
    V11_CHECK(server.snapshot() == settled, server.snapshot());
    checked++;

    context() = "full-size catalog";
    Source full;
    const std::string longText(120, 'x');
    const std::string description(250, 'd');
    std::string example = "{\"value\":\"";
    example += std::string(200, 'e');
    example += "\"}";
    std::string escaped;
    for (char c : example) escaped += c == '"' ? std::string("\\\"") : std::string(1, c);
    for (int i = 0; i < 8; i++)
        full.capabilities.push_back({NIVALO_KIND_FUNCTION, "fn" + std::to_string(i),
                                     "{\"displayName\":\"" + longText + "\",\"description\":\"" + description +
                                         "\",\"argumentExample\":\"" + escaped +
                                         "\",\"returnType\":\"integer\",\"timeoutSeconds\":600,\"dangerLevel\":\"dangerous\",\"sortOrder\":" +
                                         std::to_string(i) + "}"});
    for (int i = 0; i < 12; i++)
        full.capabilities.push_back({NIVALO_KIND_VARIABLE, "variable" + std::to_string(i),
                                     "{\"dataType\":\"number\",\"unit\":\"unit\",\"historyEnabled\":true,\"sortOrder\":" +
                                         std::to_string(i) + "}"});
    ServerCatalog fresh;
    std::unique_ptr<NivaloCatalogState> none;
    report(full, none, fresh, NIVALO_CATALOG_CHANGED, 1U, &packets);
    V11_CHECK(fresh.liveCount() == 20U && packets >= 3U, "live " << fresh.liveCount() << " packets " << packets);
    checked++;

    context() = "corrupted catalog blob";
    std::vector<uint8_t> blob(NivaloCatalogBlobBytes);
    const size_t length = nivaloEncodeCatalog(*stored, blob.data(), blob.size());
    blob[5] ^= 0x10U;
    NivaloCatalogState decoded;
    V11_CHECK(!nivaloDecodeCatalog(blob.data(), length, &decoded), "corruption detected");
    checked++;
    return checked;
}
} // namespace

int main()
{
    int checked = mergeVectorCases();
    context() = "fixture coverage";
    V11_CHECK(checked >= 20, "capability-merge cases not found");
    checked += plannerCases();
    return finish("capability catalog", checked);
}
