#ifndef NIVALO_CATALOG_H
#define NIVALO_CATALOG_H

// Capability catalog of specs/cloud-mqtt-v1.1.md section 12, owned by the firmware: the catalog
// revision advances whenever a definition changes, a name is renamed or removed, and stays when
// the same entries are reported again. Removed names are kept as tombstones and re-sent as
// removes with every refresh. The revision, a digest of the live definitions and the names are
// persisted (one blob), so the decision survives reboots and firmware updates.

#include <stddef.h>
#include <stdint.h>

#include "NivaloWire.h"

enum NivaloCapabilityKind
{
    NIVALO_KIND_VARIABLE = 0,
    NIVALO_KIND_FUNCTION = 1
};

// The registered capabilities, as the catalog sees them.
class NivaloCatalogSource
{
public:
    virtual ~NivaloCatalogSource() {}
    virtual size_t capabilityCount() const = 0;
    virtual NivaloCapabilityKind capabilityKind(size_t index) const = 0;
    virtual const char *capabilityName(size_t index) const = 0;
    // The normalized definition object (defaults written explicitly).
    virtual void writeDefinition(size_t index, NivaloJsonWriter &writer) const = 0;
};

static const size_t NivaloCatalogMaxLive = 20U;
static const size_t NivaloCatalogMaxRetired = 16U;
static const size_t NivaloCatalogBlobBytes = 2560U;
static const uint32_t NivaloCatalogMaxRevision = 2147483647UL;

struct NivaloCatalogName
{
    uint8_t kind = NIVALO_KIND_VARIABLE;
    char name[65] = {0};
};

struct NivaloCatalogState
{
    uint32_t revision = 0U;
    uint8_t digest[32] = {0};
    uint8_t liveCount = 0U;
    NivaloCatalogName live[NivaloCatalogMaxLive];
    uint8_t retiredCount = 0U;
    NivaloCatalogName retired[NivaloCatalogMaxRetired];
};

enum NivaloCatalogDecision
{
    // Same live definitions as stored: reuse the stored revision.
    NIVALO_CATALOG_UNCHANGED = 0,
    // A new revision: persist next before publishing anything.
    NIVALO_CATALOG_CHANGED = 1,
    // Too many entries, or the revision cannot advance.
    NIVALO_CATALOG_INVALID = 2
};

// Decides the revision for the current registrations. stored is NULL when nothing was persisted.
NivaloCatalogDecision nivaloPlanCatalog(const NivaloCatalogSource &source, const NivaloCatalogState *stored,
                                        NivaloCatalogState *next);

size_t nivaloEncodeCatalog(const NivaloCatalogState &state, uint8_t *output, size_t capacity);
bool nivaloDecodeCatalog(const uint8_t *data, size_t length, NivaloCatalogState *state);

// Receives one capability-operations payload ({"source":"esp32","revision":..,"ops":[..]}).
class NivaloCatalogSink
{
public:
    virtual ~NivaloCatalogSink() {}
    virtual bool emitPacket(const char *payload, size_t length) = 0;
};

// The whole catalog at state.revision: an upsert for every live entry and a remove for every
// tombstone, at most 8 operations and maxPayloadBytes per packet. A case-only rename (a removed
// name and a live case twin of one kind) always shares one packet. Returns false when a packet
// could not be built or emitted.
bool nivaloRefreshCatalog(const NivaloCatalogSource &source, const NivaloCatalogState &state,
                          size_t maxPayloadBytes, NivaloCatalogSink &sink);

#endif
