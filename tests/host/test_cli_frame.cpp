#include "NivaloCliFrame.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
void require(bool condition, const char *message)
{
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}
} // namespace

int main()
{
    char buffer[9] = {0};
    NivaloCliFrame frame(8U, 5U);

    require(frame.push('{', 10U, buffer) == NIVALO_CLI_FRAME_NONE, "first byte failed");
    require(frame.push('}', 11U, buffer) == NIVALO_CLI_FRAME_NONE, "second byte failed");
    require(frame.push('\n', 12U, buffer) == NIVALO_CLI_FRAME_READY, "complete line not ready");
    require(frame.length() == 2U && std::memcmp(buffer, "{}", 2U) == 0, "line content drifted");
    frame.reset(buffer);
    require(frame.length() == 0U && buffer[0] == '\0' && buffer[1] == '\0', "reset did not wipe line");

    require(frame.push('x', UINT32_C(0xfffffffe), buffer) == NIVALO_CLI_FRAME_NONE, "rollover start failed");
    require(frame.expire(UINT32_C(2), buffer) == NIVALO_CLI_FRAME_NONE, "frame expired before rollover timeout");
    require(frame.expire(UINT32_C(3), buffer) == NIVALO_CLI_FRAME_DROPPED, "frame survived rollover timeout");

    for (int index = 0; index < 8; ++index)
        require(frame.push('a', static_cast<unsigned long>(index), buffer) == NIVALO_CLI_FRAME_NONE, "bounded byte rejected");
    require(frame.push('b', 8U, buffer) == NIVALO_CLI_FRAME_NONE, "oversize transition failed");
    require(frame.push('\n', 9U, buffer) == NIVALO_CLI_FRAME_DROPPED, "oversized line accepted");
    frame.reset(buffer);

    require(frame.push('\0', 20U, buffer) == NIVALO_CLI_FRAME_NONE, "NUL transition failed");
    require(frame.push('\n', 21U, buffer) == NIVALO_CLI_FRAME_DROPPED, "NUL-containing line accepted");
    frame.reset(buffer);
    require(frame.push('\r', 30U, buffer) == NIVALO_CLI_FRAME_NONE, "CR handling failed");
    require(frame.push('\n', 31U, buffer) == NIVALO_CLI_FRAME_DROPPED, "empty line accepted");
    frame.reset(buffer);
    require(frame.push('{', 40U, buffer) == NIVALO_CLI_FRAME_NONE, "CRLF frame start failed");
    require(frame.push('}', 41U, buffer) == NIVALO_CLI_FRAME_NONE, "CRLF frame body failed");
    require(frame.push('\r', 42U, buffer) == NIVALO_CLI_FRAME_NONE, "CRLF marker failed");
    require(frame.push('\n', 43U, buffer) == NIVALO_CLI_FRAME_READY, "valid CRLF line rejected");
    frame.reset(buffer);
    require(frame.push('{', 50U, buffer) == NIVALO_CLI_FRAME_NONE, "embedded CR frame start failed");
    require(frame.push('\r', 51U, buffer) == NIVALO_CLI_FRAME_NONE, "embedded CR marker failed");
    require(frame.push('}', 52U, buffer) == NIVALO_CLI_FRAME_NONE, "embedded CR rejection transition failed");
    require(frame.push('\n', 53U, buffer) == NIVALO_CLI_FRAME_DROPPED, "embedded CR was silently stripped");
    return 0;
}
