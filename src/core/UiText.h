#pragma once

// On-device UI copy. CJK must fit firmware jp_12 (Jōyō + kana + extra UI glyphs).
// English / Japanese / Chinese. `apply()` after settings.language changes.
#define UI_STRINGS(X)                                                                                                 \
  X(continueReading, "Continue", "続きから", "继续阅读")                                                              \
  X(browse, "Library", "本棚", "书架")                                                                                \
  X(fileTransfer, "File Transfer", "ファイル転送", "文件传输")                                                        \
  X(settings, "Settings", "設定", "设置")                                                                             \
  X(language, "Language: %s", "言語: %s", "语言: %s")                                                                 \
  X(powerOffNone, "Power off: Off", "電源オフ: しない", "关机: 关闭")                                                 \
  X(powerOffMin, "Power off: %u min", "電源オフ: %u分", "关机: %u分钟")                                               \
  X(refreshEveryPage, "Refresh: Every page", "画面更新: 毎ページ", "刷新: 每页")                                      \
  X(refreshEveryN, "Refresh: Every %u pages", "画面更新: %uページごと", "刷新: 每%u页")                               \
  X(nightMode, "Night mode: %s", "夜間モード: %s", "夜间模式: %s")                                                    \
  X(on, "On", "オン", "开")                                                                                           \
  X(off, "Off", "オフ", "关")                                                                                         \
  X(readingFont, "Reading font", "本文フォント", "阅读字体")                                                          \
  X(tiltPageTurn, "Tilt page turn: %s", "傾きでページ送り: %s", "倾斜翻页: %s")                                        \
  X(gyroAutoOffNone, "Gyro auto-off: Off", "ジャイロ自動オフ: なし", "陀螺仪自动关闭: 关")                             \
  X(gyroAutoOffSec, "Gyro auto-off: %u sec", "ジャイロ自動オフ: %u秒", "陀螺仪自动关闭: %u秒")                         \
  X(clearCache, "Clear cache", "キャッシュ削除", "清除缓存")                                                          \
  X(clearCacheConfirm, "Clear cache: Confirm?", "キャッシュ削除: 確認?", "清除缓存: 确认?")                           \
  X(cacheCleared, "Cache cleared", "キャッシュを削除しました", "缓存已清除")                                          \
  X(updateFirmware, "Update firmware", "ファームウェア更新", "更新固件")                                              \
  X(back, "Back", "戻る", "返回")                                                                                     \
  X(serverStartFailed, "Could not start server", "サーバーを開始できません", "无法启动服务器")                        \
  X(networkSsid, "Network: %s", "ネットワーク: %s", "网络: %s")                                                        \
  X(browserHint, "Browser: books, fonts, updates", "ブラウザ: 本・フォント・更新", "浏览器: 书籍、字体、更新")         \
  X(backToStop, "Back to stop", "戻るで停止", "返回以停止")                                                           \
  X(noBinFiles, "No .bin files", ".binがありません", "没有.bin文件")                                                   \
  X(noBooks, "No books", "本がありません", "没有书籍")                                                                \
  X(backToCancel, "Back to cancel", "戻るで中止", "返回以取消")                                                        \
  X(wifi, "Wi-Fi", "Wi-Fi", "Wi-Fi")                                                                                   \
  X(lookingForSavedWifi, "Looking for saved Wi-Fi", "保存したWi-Fiを探しています", "正在查找已保存的Wi-Fi")            \
  X(scanning, "Scanning...", "スキャン中...", "正在扫描...")                                                           \
  X(confirmToPickNetwork, "Confirm to pick a network", "決定でネットワークを選ぶ", "按确认选择网络")                  \
  X(noNetworks, "No networks found", "ネットワークが見つかりません", "未找到网络")                                    \
  X(savedMark, "  [saved]", "  [保存]", "  [已保存]")                                                                 \
  X(lockedMark, "  [lock]", "  [鍵]", "  [锁定]")                                                                      \
  X(connectingTo, "Connecting to %s...", "%s に接続中...", "正在连接 %s...")                                           \
  X(connectionFailed, "Connection failed", "接続に失敗しました", "连接失败")                                          \
  X(confirmRetry, "Confirm to retry", "決定でもう一度", "按确认重试")                                                 \
  X(confirmClearPassword, "Confirm to clear password", "決定でパスワード削除", "按确认清除密码")                      \
  X(backToKeepPassword, "Back to keep password", "戻るで残す", "返回以保留密码")                                      \
  X(wifiPassword, "Wi-Fi password", "Wi-Fiパスワード", "Wi-Fi密码")                                                    \
  X(keyShift, "Shift", "シフト", "Shift")                                                                             \
  X(keySpace, "Space", "空白", "空格")                                                                                \
  X(keyDel, "Del", "削除", "删除")                                                                                    \
  X(keyDone, "Done", "完了", "完成")                                                                                  \
  X(keyGo, "Go", "移動", "跳转")                                                                                      \
  X(chapters, "Contents", "目次", "目录")                                                                             \
  X(goToPageRange, "Go to page (%lu / %u)", "ページ指定 (%lu / %u)", "跳转页码 (%lu / %u)")                            \
  X(chapterN, "Ch. %d (p%u-%u)", "第%d章 (p%u-%u)", "第%d章 (p%u-%u)")                                                 \
  X(chapterNamed, "%s (p%u-%u)", "%s (p%u-%u)", "%s (p%u-%u)")                                                         \
  X(goToPage, "Go to page", "ページ指定", "跳转页码")                                                                  \
  X(currentlyOnPage, "Currently page %lu", "現在 %lu ページ", "当前第 %lu 页")                                         \
  X(noFonts, "No fonts", "フォントがありません", "没有字体")                                                          \
  X(uploadFontHint, "Send .xgf2 via file transfer", "ファイル転送で.xgf2を送る", "通过文件传输发送.xgf2")              \
  X(updatingFirmware, "Updating", "更新しています", "正在更新")                                                       \
  X(doNotPowerOff, "Do not power off", "電源を切らないでください", "请勿关机")                                        \
  X(updateComplete, "Update complete", "更新が完了しました", "更新完成")                                              \
  X(restarting, "Restarting", "再起動します", "正在重启")                                                             \
  X(updateFailed, "Update failed", "更新に失敗しました", "更新失败")                                                  \
  X(updateFirmwareQ, "Update firmware?", "ファームウェアを更新しますか?", "要更新固件吗?")                            \
  X(confirmToFlash, "Confirm to flash", "決定で書き込み", "按确认写入")                                               \
  X(invalidFirmware, "Invalid firmware", "不正なファームウェア", "无效固件")                                          \
  X(writeFailed, "Write failed", "書き込みに失敗", "写入失败")                                                        \
  X(couldNotOpenFile, "Could not open file", "ファイルを開けません", "无法打开文件")                                  \
  X(couldNotReadFile, "Could not read file", "読み込みに失敗", "读取失败")                                            \
  X(opening, "Opening", "開いています", "正在打开")                                                                   \
  X(couldNotOpenBook, "Could not open book", "本を開けません", "无法打开书籍")                                        \
  X(outOfMemory, "Out of memory", "メモリ不足", "内存不足")                                                           \
  X(fontMissing, "Font missing", "フォントがありません", "缺少字体")                                                  \
  X(fileNotFound, "File not found", "ファイルがありません", "找不到文件")                                             \
  X(sdCardError, "SD card error", "SDカードエラー", "SD卡错误")                                                        \
  X(loading, "Loading...", "読み込み中...", "正在加载...")                                                             \
  X(invalidFormat, "Invalid format", "形式が違います", "格式无效")                                                    \
  X(unknownError, "Unknown error", "不明なエラー", "未知错误")                                                        \
  X(fileTooSmall, "File too small", "ファイルが小さい", "文件过小")                                                   \
  X(fileTooLarge, "File too large", "ファイルが大きい", "文件过大")                                                   \
  X(wrongDevice, "Wrong device", "機種が違います", "设备不匹配")

namespace uiText {

#define UI_EXTERN(name, en, ja, zh) extern const char* name;
UI_STRINGS(UI_EXTERN)
#undef UI_EXTERN

void apply();
extern const char* languageName;
const char* error(const char* en);

}  // namespace uiText
