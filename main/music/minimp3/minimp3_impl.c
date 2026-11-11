/* minimp3 单编译单元（public domain / CC0）
 * 只启用 MP3 解码，减小体积；由 mp3_stream_player.cc 使用。 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
