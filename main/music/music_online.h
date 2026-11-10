/* MusicPlayer 在线音乐扩展（自建 music_server.py）
 *
 * 单独成文件，避免把 music_player.cc 撑得过大；
 * 由 music_player.cc 在 Initialize() 中调用 RegisterOnlineMusicTools()，
 * 并在 MusicPlayer 中实现 PlayOnline / PlayOnlineById / SearchOnline。
 */
#ifndef MUSIC_ONLINE_H
#define MUSIC_ONLINE_H

#include "music_player.h"

#include <string>
#include <vector>

namespace music_online {

/** 注册 self.music.* 在线工具（在 MusicPlayer::Initialize 中调用） */
void RegisterTools(MusicPlayer& player);

/** 把服务端 JSON 里的一条曲目记入 MusicPlayer 缓存（供 by_id 播放） */
void CacheFromSearchJson(const std::string& json);

}  // namespace music_online

#endif  // MUSIC_ONLINE_H
