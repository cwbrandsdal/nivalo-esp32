#include "NivaloLinkReceiveSequence.h"

#include <cassert>

int main()
{
    NivaloLinkReceiveSequence sequences;

    assert(sequences.acknowledgment() == 0U);
    assert(sequences.observe(0U));
    assert(!sequences.observe(0U));
    assert(sequences.acknowledgment() == 0U);

    assert(sequences.observe(65535U));
    assert(!sequences.observe(65535U));
    assert(sequences.acknowledgment() == 65535U);

    assert(sequences.observe(0U));
    assert(!sequences.observe(0U));

    sequences.reset();
    assert(sequences.observe(0U));
    return 0;
}
