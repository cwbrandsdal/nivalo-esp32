#include "NivaloProvisioningTimePolicy.h"

#include <cstdint>
#include <cstdlib>

namespace
{
void require(bool condition)
{
    if (!condition) std::abort();
}
}

int main()
{

    require(!NivaloProvisioningTimePolicy::elapsed(14999U, 0U, 15000U));
    require(NivaloProvisioningTimePolicy::elapsed(15000U, 0U, 15000U));
    require(NivaloProvisioningTimePolicy::elapsed(10U, UINT32_MAX - 9U, 20U));

    require(NivaloProvisioningTimePolicy::mayRetry(1U, 3U));
    require(NivaloProvisioningTimePolicy::mayRetry(2U, 3U));
    require(!NivaloProvisioningTimePolicy::mayRetry(3U, 3U));
    require(NivaloProvisioningTimePolicy::retryDelay(1U, 2000U, 30000U) == 2000U);
    require(NivaloProvisioningTimePolicy::retryDelay(2U, 2000U, 30000U) == 4000U);
    require(NivaloProvisioningTimePolicy::retryDelay(8U, 2000U, 30000U) == 30000U);
    require(!NivaloProvisioningTimePolicy::retryDue(UINT32_MAX - 10U, 5U));
    require(NivaloProvisioningTimePolicy::retryDue(5U, 5U));
    require(NivaloProvisioningTimePolicy::retryDue(10U, UINT32_MAX - 5U));
    return 0;
}
