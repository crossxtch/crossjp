#pragma once

// On-device UI copy. Japanese, with Latin left in the strings that need it
// (Wi-Fi, .bin, .xgf2). CJK must fit firmware jp_12. Latin is Ubuntu.
#define UI_STRINGS(X)                                                                                                 \
  X(continueReading, "続きから") \
  X(browse, "本棚") \
  X(fileTransfer, "ファイル転送") \
  X(settings, "設定") \
  X(powerOffNone, "電源オフ: しない") \
  X(powerOffMin, "電源オフ: %u分") \
  X(refreshEveryPage, "画面更新: 毎ページ") \
  X(refreshEveryN, "画面更新: %uページごと") \
  X(nightMode, "夜間モード: %s") \
  X(on, "オン") \
  X(off, "オフ") \
  X(readingFont, "本文フォント") \
  X(tiltPageTurn, "傾きでページ送り: %s") \
  X(gyroAutoOffNone, "ジャイロ自動オフ: なし") \
  X(gyroAutoOffSec, "ジャイロ自動オフ: %u秒") \
  X(clearCache, "キャッシュ削除") \
  X(clearCacheConfirm, "キャッシュ削除: 確認?") \
  X(cacheCleared, "キャッシュを削除しました") \
  X(updateFirmware, "ファームウェア更新") \
  X(back, "戻る") \
  X(serverStartFailed, "サーバーを開始できません") \
  X(networkSsid, "ネットワーク: %s") \
  X(browserHint, "ブラウザ: 本・フォント・更新") \
  X(backToStop, "戻るで停止") \
  X(noBinFiles, ".binがありません") \
  X(noBooks, "本がありません") \
  X(backToCancel, "戻るで中止") \
  X(wifi, "Wi-Fi") \
  X(lookingForSavedWifi, "保存したWi-Fiを探しています") \
  X(scanning, "スキャン中...") \
  X(confirmToPickNetwork, "決定でネットワークを選ぶ") \
  X(noNetworks, "ネットワークが見つかりません") \
  X(savedMark, "  [保存]") \
  X(lockedMark, "  [鍵]") \
  X(connectingTo, "%s に接続中...") \
  X(connectionFailed, "接続に失敗しました") \
  X(confirmRetry, "決定でもう一度") \
  X(confirmClearPassword, "決定でパスワード削除") \
  X(backToKeepPassword, "戻るで残す") \
  X(wifiPassword, "Wi-Fiパスワード") \
  X(keyShift, "シフト") \
  X(keySpace, "空白") \
  X(keyDel, "削除") \
  X(keyDone, "完了") \
  X(keyGo, "移動") \
  X(chapters, "目次") \
  X(goToPageRange, "ページ指定 (%lu / %u)") \
  X(chapterN, "第%d章 (p%u-%u)") \
  X(chapterNamed, "%s (p%u-%u)") \
  X(goToPage, "ページ指定") \
  X(currentlyOnPage, "現在 %lu ページ") \
  X(noFonts, "フォントがありません") \
  X(uploadFontHint, "ファイル転送で.xgf2を送る") \
  X(updatingFirmware, "更新しています") \
  X(doNotPowerOff, "電源を切らないでください") \
  X(updateComplete, "更新が完了しました") \
  X(restarting, "再起動します") \
  X(updateFailed, "更新に失敗しました") \
  X(updateFirmwareQ, "ファームウェアを更新しますか?") \
  X(confirmToFlash, "決定で書き込み") \
  X(invalidFirmware, "不正なファームウェア") \
  X(writeFailed, "書き込みに失敗") \
  X(couldNotOpenFile, "ファイルを開けません") \
  X(couldNotReadFile, "読み込みに失敗") \
  X(opening, "開いています") \
  X(couldNotOpenBook, "本を開けません") \
  X(outOfMemory, "メモリ不足") \
  X(fontMissing, "フォントがありません") \
  X(fileNotFound, "ファイルがありません") \
  X(sdCardError, "SDカードエラー") \
  X(loading, "読み込み中...") \
  X(invalidFormat, "形式が違います") \
  X(unknownError, "不明なエラー") \
  X(fileTooSmall, "ファイルが小さい") \
  X(fileTooLarge, "ファイルが大きい") \
  X(wrongDevice, "機種が違います")

namespace uiText {

#define UI_EXTERN(name, text) extern const char* name;
UI_STRINGS(UI_EXTERN)
#undef UI_EXTERN

// Map an internal English error key to the Japanese UI string.
const char* error(const char* en);

}  // namespace uiText
