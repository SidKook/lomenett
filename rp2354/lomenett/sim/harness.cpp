// harness.cpp -- UI simulator: compiles the real ui.h/vault.h against a mock
// display, renders every screen in both orientations to out/*.ppm, and
// reports text overlap or clipping. Run via ./build.sh.
#include "Arduino.h"
#include "Adafruit_GFX.h"
#include "Adafruit_SSD1306.h"
#include "Wire.h"
#include "LittleFS.h"
#include "lomenett_logic.h"
#include "ui.h"
#include <vector>
#include <string>
#include <cstdio>

SerialT Serial;
TwoWire Wire;
FS LittleFS;
uint32_t g_sim_millis = 1000;
// Defined in the sketch; never called by the render path.
static bool t1LinkReset() { return false; }
static int t1SendApdu(const uint8_t *, int, uint8_t *, int) { return -1; }

static Adafruit_SSD1306 oled(128, 32, nullptr, -1);
static Ui ui;   // the harness drives ui.screen directly

// ---------- fake vault population ----------
static void clearVault() { vault.count = 0; vaultBlob = ""; vaultUi = VaultUi(); }
static void addPw(const char *name, const char *user, const char *pw, int tabs = 0) {
  VaultEntry &e = vault.entries[vault.count++];
  memset(&e, 0, sizeof(e));
  strncpy(e.name, name, VAULT_NAME_LEN); strncpy(e.username, user, VAULT_USER_LEN);
  strncpy(e.secret, pw, VAULT_PW_LEN); e.tabCount = tabs; e.kind = 0;
}
static void addCard(const char *name, const char *pan, const char *exp, const char *cvv) {
  VaultEntry &e = vault.entries[vault.count++];
  memset(&e, 0, sizeof(e));
  strncpy(e.name, name, VAULT_NAME_LEN); strncpy(e.secret, pan, VAULT_PW_LEN);
  strncpy(e.expiry, exp, 5); strncpy(e.cvv, cvv, 4); e.kind = 1;
}

static void addTotp(const char *name, const char *secret, int digits = 6, int nbk = 0) {
  VaultEntry &e = vault.entries[vault.count++];
  memset(&e, 0, sizeof(e));
  strncpy(e.name, name, VAULT_NAME_LEN); strncpy(e.secret, secret, VAULT_PW_LEN);
  snprintf(e.expiry, sizeof(e.expiry), "%d", digits); strcpy(e.cvv, "30"); e.kind = KIND_TOTP; e.backupCodeCount = nbk;
}

// ---------- render one scenario ----------
struct Result { std::string name; bool overlap; bool clip; };
static std::vector<Result> results;

static bool boxesIntersect(const SimBox &a, const SimBox &b) {
  return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}

static void renderScreen(const std::string &name, int rot, int battPct, bool onUsb, bool locked) {
  oled.setRotation(rot);
  ui.locked = locked;
  // settle animations: call the frame ~40 times advancing the clock
  for (int i = 0; i < 40; i++) {
    g_sim_millis += 16;
    uiFrame(oled, ui, 0, false, false, battPct, onUsb, 3.9f, g_sim_millis);
  }
  // overlap: any two text runs from different setCursor calls that intersect
  bool overlap = false;
  for (size_t i = 0; i < oled.textRuns.size() && !overlap; i++)
    for (size_t j = i + 1; j < oled.textRuns.size(); j++)
      if (oled.textRuns[i].run != oled.textRuns[j].run && boxesIntersect(oled.textRuns[i], oled.textRuns[j])) { overlap = true; break; }
  bool clip = oled.textClip > 0;
  results.push_back({name, overlap, clip});
  // write PPM (physical 128x32, 1 bpp -> white on black)
  std::string fn = std::string("out/") + name + ".ppm";
  FILE *f = fopen(fn.c_str(), "wb");
  fprintf(f, "P6\n128 32\n255\n");
  for (int y = 0; y < 32; y++) for (int x = 0; x < 128; x++) {
    int on = (oled.buf[x + (y / 8) * 128] >> (y & 7)) & 1;
    unsigned char v = on ? 255 : 0; fputc(v, f); fputc(v, f); fputc(v, f);
  }
  fclose(f);
}

static void resetUi() { ui = Ui(); ui.locked = false; ui.introStartedAt = 0; vaultUi = VaultUi(); vaultUi.showLetterRing = false; }
static void fillSampleVault(int count) {
  clearVault();
  for (int i = 0; i < count; i++) { char name[10]; snprintf(name, sizeof(name), "site%02d", i); addPw(name, "user@mail", "pw"); }
}
// Same UI state rendered landscape (_land) and portrait (_port).
static void renderBothOrientations(const std::string &name, int batteryPercent = 80, bool onUsb = true, bool locked = false) {
  Ui savedUi = ui; VaultUi savedVaultUi = vaultUi;
  renderScreen(name + "_land", 0, batteryPercent, onUsb, locked);
  ui = savedUi; vaultUi = savedVaultUi;
  renderScreen(name + "_port", 1, batteryPercent, onUsb, locked);
}

int main() {
  // ---- HOME + battery states
  resetUi(); fillSampleVault(3); ui.screen = UI_HOME; renderBothOrientations("home");
  resetUi(); ui.screen = UI_HOME; renderScreen("home_battlow_land", 0, 7, false, false);
  resetUi(); ui.screen = UI_HOME; renderScreen("home_full_land", 0, 100, true, false);
  resetUi(); ui.screen = UI_HOME; renderScreen("home_full_port", 1, 100, true, false);
  // ---- PIN
  resetUi(); ui.screen = UI_PIN; ui.pinMode = PIN_UNLOCK; renderBothOrientations("pin_enter", 55, false, true);
  resetUi(); ui.screen = UI_PIN; ui.pinMode = PIN_NEW; renderScreen("pin_new_land", 0, 55, false, true);
  resetUi(); ui.screen = UI_PIN; ui.pinMode = PIN_BACKUP; renderScreen("pin_backup_land", 0, 55, false, true);
  // ---- TYPER: empty / one / long / long labels / favourite / picking
  resetUi(); clearVault(); uiGo(ui, UI_VAULT); renderBothOrientations("typer_empty");
  resetUi(); clearVault(); addPw("github", "octocat", "x"); uiGo(ui, UI_VAULT); renderBothOrientations("typer_one");
  resetUi(); fillSampleVault(20); uiGo(ui, UI_VAULT); ui.menus[UI_VAULT].selected = 7; ui.menus[UI_VAULT].position = 7; renderBothOrientations("typer_long");
  resetUi(); clearVault(); addPw("averylonglabelname12345", "averylongusername@example.com", "pw"); addPw("b", "u", "p"); uiGo(ui, UI_VAULT); renderBothOrientations("typer_longlabel");
  resetUi(); fillSampleVault(5); vault.entries[3].favorite = 1; uiGo(ui, UI_VAULT); renderScreen("typer_fav_land", 0, 80, true, false);
  resetUi(); fillSampleVault(5); vault.entries[0].icon = 1; vault.entries[1].icon = 2; vault.entries[2].icon = 97; vault.entries[3].icon = 120; vault.entries[1].favorite = 1; uiGo(ui, UI_VAULT); ui.menus[UI_VAULT].selected = 1; ui.menus[UI_VAULT].position = 1; renderBothOrientations("typer_icons");
  resetUi(); fillSampleVault(5); uiGo(ui, UI_VAULT); ui.menus[UI_VAULT].selected = 5; ui.menus[UI_VAULT].position = 5; renderBothOrientations("typer_backitem");
  resetUi(); clearVault(); addPw("amazon","a","p"); addPw("bank","b","p"); addPw("github","g","p"); addPw("gmail","g","p"); addPw("zoom","z","p"); addPw("123site","n","p"); uiGo(ui, UI_VAULT); vaultUi.showLetterRing = true; renderScreen("typer_letters_port", 1, 80, true, false);
  resetUi(); clearVault(); addPw("amazon","a","p"); addPw("bank","b","p"); addPw("github","g","p"); uiGo(ui, UI_VAULT); vaultUi.showLetterRing = true; vaultUi.letterMenu.selected = 3; vaultUi.letterMenu.position = 3; renderScreen("typer_letters_back_port", 1, 80, true, false);
  resetUi(); fillSampleVault(3); ui.entryIndex = 1; uiGo(ui, UI_ICONPICK); ui.menus[UI_ICONPICK].selected = 3; ui.menus[UI_ICONPICK].position = 3; renderBothOrientations("iconpick");
  resetUi(); fillSampleVault(3); ui.entryIndex = 1; uiGo(ui, UI_ICONPICK); ui.menus[UI_ICONPICK].selected = 96; ui.menus[UI_ICONPICK].position = 96; renderBothOrientations("iconpick_letter");
  resetUi(); fillSampleVault(3); ui.entryIndex = 1; vault.entries[1].icon = 40; uiGo(ui, UI_PWSET); renderBothOrientations("pwset_icon");
  resetUi(); fillSampleVault(5); ui.pickingForPasswordChange = true; uiGo(ui, UI_VAULT); renderScreen("typer_pick_land", 0, 80, true, false);
  resetUi(); fillSampleVault(5); uiGo(ui, UI_VAULT); vaultUi.isTyping = true; vaultUi.charsTyped = 5; vaultUi.charsTotal = 12; renderScreen("typer_typing_land", 0, 80, true, false);
  // ---- CARDS: empty / list
  resetUi(); clearVault(); uiGo(ui, UI_CARDS); renderBothOrientations("cards_empty");
  resetUi(); clearVault(); addCard("Visa", "4111111111111111", "08/28", "123"); addCard("Amex", "378282246310005", "01/29", "4321"); uiGo(ui, UI_CARDS); renderBothOrientations("cards");
  // ---- 2FA: home item, empty list, list, code (no clock / 6 / 8 digits)
  resetUi(); fillSampleVault(3); ui.screen = UI_HOME; ui.menus[UI_HOME].selected = 2; ui.menus[UI_HOME].position = 2; renderBothOrientations("home_2fa");
  resetUi(); clearVault(); uiGo(ui, UI_CODES); renderBothOrientations("codes_empty");
  resetUi(); clearVault(); addPw("github", "octocat", "x"); addTotp("github", "JBSWY3DPEHPK3PXP"); addTotp("Google", "GEZDGNBVGY3TQOJQ"); uiGo(ui, UI_CODES); renderBothOrientations("codes");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODE); renderBothOrientations("code_noclock");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); uiGo(ui, UI_CODES); uiGo(ui, UI_CLOCKASK); renderBothOrientations("clockask");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODE); uiGo(ui, UI_CLOCKSYNC); renderBothOrientations("clocksync_linux");
  resetUi(); clearVault(); addPw("github.com","octocat","p"); addPw("github.com 2","someone.long@example.com","p"); uiOfferLogin(ui, "github.com", 2); renderBothOrientations("login_two");
  resetUi(); clearVault(); addPw("accounts.google.com","me@gmail.com","p"); uiOfferLogin(ui, "accounts.google.com", 0); renderBothOrientations("login_one");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODE); uiGo(ui, UI_CLOCKSYNC); ui.menus[UI_CLOCKSYNC].selected = 2; renderBothOrientations("clocksync_windows");
  totpClockSet(1793836812ULL);
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODE); renderBothOrientations("code");
  resetUi(); clearVault(); addTotp("averylonglabelname12345", "GEZDGNBVGY3TQOJQ", 8); ui.entryIndex = 0; uiGo(ui, UI_CODE); renderBothOrientations("code_8digit");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODE); ui.menus[UI_CODE].selected = 1; renderBothOrientations("code_settings");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP", 6, 3); ui.entryIndex = 0; uiGo(ui, UI_CODE); ui.menus[UI_CODE].selected = 1; renderBothOrientations("code_backup_page");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP", 6, 3); ui.entryIndex = 0; uiGo(ui, UI_CODE); ui.menus[UI_CODE].selected = 2; renderBothOrientations("code_settings_4pg");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP", 6, 3); ui.entryIndex = 0; uiGo(ui, UI_CODE); ui.menus[UI_CODE].selected = 3; renderBothOrientations("code_back");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODESET); renderBothOrientations("codeset_icon");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); vault.entries[0].pressEnter = 1; ui.entryIndex = 0; uiGo(ui, UI_CODESET); ui.menus[UI_CODESET].selected = 1; renderBothOrientations("codeset_enter");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODESET); ui.menus[UI_CODESET].selected = 2; renderBothOrientations("codeset_fav");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODESET); ui.menus[UI_CODESET].selected = 3; renderBothOrientations("codeset_delete");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; ui.confirmKind = 1; uiGo(ui, UI_CONFIRM); renderBothOrientations("codeset_confirm");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP", 6, 5); ui.entryIndex = 0; uiGo(ui, UI_BACKUPS); ui.menus[UI_BACKUPS].selected = 4; renderBothOrientations("backups");
  resetUi(); clearVault(); addTotp("github", "JBSWY3DPEHPK3PXP"); ui.entryIndex = 0; uiGo(ui, UI_CODE); vaultUi.isTyping = true; vaultUi.charsTyped = 2; vaultUi.charsTotal = 6; renderScreen("code_typing_land", 0, 80, true, false);
  // ---- per-entry menus
  resetUi(); fillSampleVault(3); ui.entryIndex = 1; uiGo(ui, UI_PWMENU); renderBothOrientations("pwmenu");
  resetUi(); fillSampleVault(3); ui.entryIndex = 1; uiGo(ui, UI_PWMENU); ui.menus[UI_PWMENU].selected = 2; vaultUi.isTyping = true; vaultUi.charsTyped = 3; vaultUi.charsTotal = 9; renderScreen("pwmenu_typing_land", 0, 80, true, false);
  resetUi(); fillSampleVault(3); ui.entryIndex = 1; vault.entries[1].tabCount = 2; vault.entries[1].pressEnter = 1; uiGo(ui, UI_PWSET); renderBothOrientations("pwset_tabs");
  resetUi(); fillSampleVault(3); ui.entryIndex = 1; uiGo(ui, UI_PWSET); ui.menus[UI_PWSET].selected = 2; renderBothOrientations("pwset_fav");
  resetUi(); fillSampleVault(3); ui.entryIndex = 1; uiGo(ui, UI_PWSET); ui.menus[UI_PWSET].selected = 3; renderBothOrientations("pwset_chgpw");
  resetUi(); clearVault(); addCard("Visa", "4111111111111111", "08/28", "123"); ui.entryIndex = 0; uiGo(ui, UI_CARDMENU); renderBothOrientations("cardmenu");
  resetUi(); clearVault(); addCard("Visa", "4111111111111111", "08/28", "123"); ui.entryIndex = 0; uiGo(ui, UI_CARDMENU); ui.menus[UI_CARDMENU].selected = 1; renderBothOrientations("cardmenu_exp");
  // ---- SETTINGS: first, the value items, last
  resetUi(); uiGo(ui, UI_SETTINGS); renderBothOrientations("settings");
  resetUi(); uiGo(ui, UI_SETTINGS); ui.menus[UI_SETTINGS].selected = 6; renderBothOrientations("settings_autooff");
  resetUi(); uiGo(ui, UI_SETTINGS); ui.menus[UI_SETTINGS].selected = 6; ui.autoOffSeconds = 0; renderBothOrientations("settings_autooff_never");
  resetUi(); uiGo(ui, UI_SETTINGS); ui.menus[UI_SETTINGS].selected = 7; ui.autoWipe = true; renderScreen("settings_wipe_land", 0, 80, true, false);
  resetUi(); uiGo(ui, UI_SETTINGS); ui.menus[UI_SETTINGS].selected = 9; renderBothOrientations("settings_reset");
  resetUi(); uiGo(ui, UI_SETTINGS); ui.menus[UI_SETTINGS].selected = 11; renderScreen("settings_back_land", 0, 80, true, false);
  // ---- change password / new entry / confirm / info / edit / msg
  resetUi(); fillSampleVault(3); ui.changeEntryIndex = 0; uiGo(ui, UI_CHGPW); renderBothOrientations("chgpw");
  resetUi(); fillSampleVault(3); ui.changeEntryIndex = 0; uiGo(ui, UI_CHGPW); ui.menus[UI_CHGPW].selected = 2; renderScreen("chgpw_again_land", 0, 80, true, false);
  resetUi(); uiGo(ui, UI_NEWLABEL); strcpy(ui.newLabel, "GITHU"); ui.newLabelLength = 5; renderBothOrientations("newlabel");
  resetUi(); uiGo(ui, UI_NEWLABEL); strcpy(ui.newLabel, "AVERYLONGLABELNAME123"); ui.newLabelLength = 21; ui.builderSelected = 38; renderBothOrientations("newlabel_long");
  resetUi(); uiGo(ui, UI_NEWLABEL); ui.builderSelected = 37; renderScreen("newlabel_ok_land", 0, 80, true, false);
  resetUi(); strcpy(ui.newLabel, "VISA"); uiGo(ui, UI_NEWTYPE); renderBothOrientations("newtype");
  resetUi(); strcpy(ui.newLabel, "VISA"); uiGo(ui, UI_NEWNUM); strcpy(ui.newCardNumber, "4111111111111111"); renderBothOrientations("newnum");
  resetUi(); strcpy(ui.newLabel, "VISA"); uiGo(ui, UI_NEWNUM); ui.newCardStage = 1; strcpy(ui.newCardExpiry, "08"); renderScreen("newexp_land", 0, 80, true, false);
  resetUi(); ui.confirmKind = 1; uiGo(ui, UI_CONFIRM); renderBothOrientations("confirm_delete");
  resetUi(); ui.confirmKind = 0; uiGo(ui, UI_CONFIRM); ui.menus[UI_CONFIRM].selected = 1; renderScreen("confirm_reset_yes_land", 0, 80, true, false);
  resetUi(); uiGo(ui, UI_INFO); ui.infoLine1 = "SE050 v7.2.0  0x0102"; ui.infoLine2 = "fw 2026-09-14"; renderBothOrientations("info");
  resetUi(); uiGo(ui, UI_EDITMODE); ui.editState = 0; renderBothOrientations("edit");
  resetUi(); uiGo(ui, UI_EDITMODE); ui.editState = 3; renderScreen("edit_ejected_land", 0, 80, true, false);
  resetUi(); uiToast(ui, "CREATED", g_sim_millis + 100000, UI_HOME); renderBothOrientations("msg");
  resetUi(); uiToast(ui, "IMP 299 SKIP 1", g_sim_millis + 100000, UI_HOME); renderScreen("msg_long_land", 0, 80, true, false);

  // ---- report
  int fails = 0;
  printf("%-24s %-8s %-8s\n", "screen", "overlap", "clip");
  for (auto &r : results) {
    bool bad = r.overlap || r.clip;
    printf("%-24s %-8s %-8s %s\n", r.name.c_str(), r.overlap ? "OVERLAP" : "ok", r.clip ? "CLIP" : "ok", bad ? "  <-- FAIL" : "");
    if (bad) fails++;
  }
  printf("\n%zu screens, %d with issues\n", results.size(), fails);
  return fails ? 1 : 0;
}
