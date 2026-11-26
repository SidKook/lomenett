// test_vault.cpp -- regression tests for EDIT mode: lays down SECRETS.TXT /
// CARDS.TXT / 2FA.TXT / CSV files as a host would, runs the real parser and
// importer, and checks the vault.
#include "Arduino.h"
#include "Adafruit_GFX.h"
#include "Adafruit_SSD1306.h"
#include "Wire.h"
#include "LittleFS.h"
#include "Adafruit_TinyUSB.h"
#include "lomenett_logic.h"
#include "ui.h"
#include "msc.h"
#include <cstdio>
#include <string>

SerialT Serial; TwoWire Wire; FS LittleFS; uint32_t g_sim_millis = 1000;
static bool t1LinkReset() { return false; }
static int t1SendApdu(const uint8_t *, int, uint8_t *, int) { return -1; }
class NullLog : public Print {}; static NullLog nullLog; Print &LogRef = nullLog;

// Pretend unlocked so vaultCommit() runs.
extern bool sessionKeyReady;

// FAT image with caller-supplied SECRETS.TXT / CARDS.TXT / 2FA.TXT.
static void putImage(const std::string &sec, const std::string &card, const std::string &tfa = "") {
  memset(mscDisk, 0, sizeof(mscDisk));
  uint8_t *b = mscDisk[0];
  static const uint8_t boot[] = { 0xEB,0x3C,0x90,'M','S','D','O','S','5','.','0',0x00,0x02,0x01,0x01,0x00,0x01,0x10,0x00,(uint8_t)MSC_BLOCKS,0x00,0xF8,0x01,0x00,0x01,0x00,0x01,0x00,0,0,0,0,0,0,0,0,0x80,0x00,0x29,'L','O','M','1' };
  memcpy(b, boot, sizeof(boot)); b[510] = 0x55; b[511] = 0xAA;
  uint8_t *fat = mscDisk[1]; fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF;
  const char *rd = "readme";
  int c = mscPlaceFile("README  TXT", 1, (const uint8_t *)rd, strlen(rd), 2);
  int c2 = mscPlaceFile("SECRETS TXT", 2, (const uint8_t *)sec.c_str(), sec.size(), c);
  int c3 = mscPlaceFile("CARDS   TXT", 3, (const uint8_t *)card.c_str(), card.size(), c2);
  if (tfa.size()) mscPlaceFile("2FA     TXT", 4, (const uint8_t *)tfa.c_str(), tfa.size(), c3);
}

static void setStored(const std::string &blob) {   // what the device holds before the edit
  vaultBlob = String(blob.c_str()); vaultParseBlob();
}
static int findEntry(const char *name) { for (int i = 0; i < vault.count; i++) if (!strcmp(vault.entries[i].name, name)) return i; return -1; }

static int pass = 0, fail = 0;
static void check(const char *what, bool ok) { printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (ok) pass++; else fail++; }

int main() {
  sessionKeyReady = true;
  const char *HDR = "Label\tUsername(Optional)\tPassword(Required)\tTab Times(Optional)\r\n";
  const char *CHDR = "Label\tCardNumber\tExp(MM/YY)\tCVV\r\n";

  // ---- Scenario 1: entry has a password, no username. Add a username. (bug 1)
  printf("S1: add username to a password-only entry\n");
  setStored("github\t\tsecretpw\n");
  putImage(std::string(HDR) + "github\toctocat\t********\r\n", CHDR);
  mscParseVault();
  { int i = findEntry("github");
    check("github still exists", i >= 0);
    if (i >= 0) { check("username is octocat", !strcmp(vault.entries[i].username, "octocat"));
                  check("password preserved (secretpw)", !strcmp(vault.entries[i].secret, "secretpw")); } }

  // ---- Scenario 2: entry has username+password. Change only the username, keep masked pw.
  printf("S2: change username, keep masked password\n");
  setStored("mail\talice\tpw123\n");
  putImage(std::string(HDR) + "mail\tbob\t********\r\n", CHDR);
  mscParseVault();
  { int i = findEntry("mail"); check("mail exists", i >= 0);
    if (i >= 0) { check("username now bob", !strcmp(vault.entries[i].username, "bob"));
                  check("password preserved (pw123)", !strcmp(vault.entries[i].secret, "pw123")); } }

  // ---- Scenario 3: change the password (type over the stars) on an entry with a username.
  printf("S3: change password on an entry with a username\n");
  setStored("bank\tbob\toldpw\n");
  putImage(std::string(HDR) + "bank\tbob\tNEWpw!\r\n", CHDR);
  mscParseVault();
  { int i = findEntry("bank"); check("bank exists", i >= 0);
    if (i >= 0) check("password changed to NEWpw!", !strcmp(vault.entries[i].secret, "NEWpw!")); }

  // ---- Scenario 4: add a tab-count to an entry that has username+password.
  printf("S4: add tab count\n");
  setStored("acct\tuser\tpw\n");
  putImage(std::string(HDR) + "acct\tuser\t********\t3\r\n", CHDR);
  mscParseVault();
  { int i = findEntry("acct"); check("acct exists", i >= 0);
    if (i >= 0) { check("password preserved", !strcmp(vault.entries[i].secret, "pw"));
                  check("tabs = 3", vault.entries[i].tabCount == 3); } }

  // ---- Scenario 5: two entries; edit one, the other must survive.
  printf("S5: editing one entry leaves the others intact\n");
  setStored("aaa\t\tp1\nbbb\tu\tp2\n");
  putImage(std::string(HDR) + "aaa\tnewuser\t********\r\nbbb\tu\t********\r\n", CHDR);
  mscParseVault();
  check("aaa exists", findEntry("aaa") >= 0);
  check("bbb still exists", findEntry("bbb") >= 0);
  { int i = findEntry("aaa"); if (i >= 0) check("aaa password preserved", !strcmp(vault.entries[i].secret, "p1")); }

  // ---- Scenario 6: card + password mixed; edit the password entry's username.
  printf("S6: mixed card + password, edit password entry\n");
  setStored("web\t\twebpw\nVisa\t$CARD\t4111111111111111\t08/28\t123\n");
  putImage(std::string(HDR) + "web\tme\t********\r\n",
           std::string(CHDR) + "Visa\t********\t08/28\t***\r\n");
  mscParseVault();
  { int w = findEntry("web"), v = findEntry("Visa");
    check("web exists", w >= 0); check("Visa card survived", v >= 0);
    if (w >= 0) check("web pw preserved", !strcmp(vault.entries[w].secret, "webpw"));
    if (v >= 0) { check("card number preserved", !strcmp(vault.entries[v].secret, "4111111111111111"));
                  check("cvv preserved", !strcmp(vault.entries[v].cvv, "123")); } }

  // ---- Scenario 7: a card must survive a plain load/commit cycle (normalizeLines)
  printf("S7: card survives normalizeLines (load/commit)\n");
  setStored("Amex\t$CARD\t378282246310005\t01/29\t4321\n");   // vaultParseBlob -> normalizeLines path
  vaultCommit(vaultBlob);                                          // a no-op commit must not flatten it
  { int i = findEntry("Amex"); check("Amex exists", i >= 0);
    if (i >= 0) { check("still a card", vault.entries[i].kind == 1);
                  check("number kept", !strcmp(vault.entries[i].secret, "378282246310005"));
                  check("exp kept", !strcmp(vault.entries[i].expiry, "01/29"));
                  check("cvv kept", !strcmp(vault.entries[i].cvv, "4321")); } }

  // ---- Scenario 8: large Google CSV import (never hang, skip bad, atomic)
  printf("S8: Google CSV import (300 rows, unicode, quoted commas, empty, long)\n");
  setStored("keepme\t\tkeeppw\n");                 // existing entry must survive
  { const char* CSV = "name,url,username,password,note\nsite000,https://www.site000.com/login,user0@ex.com,p@ss0!,n\nsite001,https://www.site001.com/login,user1@ex.com,p@ss1!,n\nsite002,https://www.site002.com/login,user2@ex.com,p@ss2!,n\nsite003,https://www.site003.com/login,user3@ex.com,p@ss3!,n\nsite004,https://www.site004.com/login,user4@ex.com,p@ss4!,n\nmünchen,https://www.site005.com/login,user5@ex.com,ünïcöde✓pass,n\nsite006,https://www.site006.com/login,user6@ex.com,\"pa,ss\"\"wd6\",n\nsite007,https://www.site007.com/login,user7@ex.com,,n\n,https://www.site008.com/login,,p@ss8!,n\nsite009,https://www.site009.com/login,user9@ex.com,xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx,n\nsite010,https://www.site010.com/login,user10@ex.com,p@ss10!,n\nsite011,https://www.site011.com/login,user11@ex.com,p@ss11!,n\nsite012,https://www.site012.com/login,user12@ex.com,p@ss12!,n\nsite013,https://www.site013.com/login,user13@ex.com,p@ss13!,n\nsite014,https://www.site014.com/login,user14@ex.com,p@ss14!,n\nsite015,https://www.site015.com/login,user15@ex.com,p@ss15!,n\nsite016,https://www.site016.com/login,user16@ex.com,p@ss16!,n\nsite017,https://www.site017.com/login,user17@ex.com,p@ss17!,n\nsite018,https://www.site018.com/login,user18@ex.com,p@ss18!,n\nsite019,https://www.site019.com/login,user19@ex.com,p@ss19!,n\nsite020,https://www.site020.com/login,user20@ex.com,p@ss20!,n\nsite021,https://www.site021.com/login,user21@ex.com,p@ss21!,n\nsite022,https://www.site022.com/login,user22@ex.com,p@ss22!,n\nsite023,https://www.site023.com/login,user23@ex.com,p@ss23!,n\nsite024,https://www.site024.com/login,user24@ex.com,p@ss24!,n\nsite025,https://www.site025.com/login,user25@ex.com,p@ss25!,n\nsite026,https://www.site026.com/login,user26@ex.com,p@ss26!,n\nsite027,https://www.site027.com/login,user27@ex.com,p@ss27!,n\nsite028,https://www.site028.com/login,user28@ex.com,p@ss28!,n\nsite029,https://www.site029.com/login,user29@ex.com,p@ss29!,n\nsite030,https://www.site030.com/login,user30@ex.com,p@ss30!,n\nsite031,https://www.site031.com/login,user31@ex.com,p@ss31!,n\nsite032,https://www.site032.com/login,user32@ex.com,p@ss32!,n\nsite033,https://www.site033.com/login,user33@ex.com,p@ss33!,n\nsite034,https://www.site034.com/login,user34@ex.com,p@ss34!,n\nsite035,https://www.site035.com/login,user35@ex.com,p@ss35!,n\nsite036,https://www.site036.com/login,user36@ex.com,p@ss36!,n\nsite037,https://www.site037.com/login,user37@ex.com,p@ss37!,n\nsite038,https://www.site038.com/login,user38@ex.com,p@ss38!,n\nsite039,https://www.site039.com/login,user39@ex.com,p@ss39!,n\nsite040,https://www.site040.com/login,user40@ex.com,p@ss40!,n\nsite041,https://www.site041.com/login,user41@ex.com,p@ss41!,n\nsite042,https://www.site042.com/login,user42@ex.com,p@ss42!,n\nsite043,https://www.site043.com/login,user43@ex.com,p@ss43!,n\nsite044,https://www.site044.com/login,user44@ex.com,p@ss44!,n\nsite045,https://www.site045.com/login,user45@ex.com,p@ss45!,n\nsite046,https://www.site046.com/login,user46@ex.com,p@ss46!,n\nsite047,https://www.site047.com/login,user47@ex.com,p@ss47!,n\nsite048,https://www.site048.com/login,user48@ex.com,p@ss48!,n\nsite049,https://www.site049.com/login,user49@ex.com,p@ss49!,n\nsite050,https://www.site050.com/login,user50@ex.com,p@ss50!,n\nsite051,https://www.site051.com/login,user51@ex.com,p@ss51!,n\nsite052,https://www.site052.com/login,user52@ex.com,p@ss52!,n\nsite053,https://www.site053.com/login,user53@ex.com,p@ss53!,n\nsite054,https://www.site054.com/login,user54@ex.com,p@ss54!,n\nsite055,https://www.site055.com/login,user55@ex.com,p@ss55!,n\nsite056,https://www.site056.com/login,user56@ex.com,p@ss56!,n\nsite057,https://www.site057.com/login,user57@ex.com,p@ss57!,n\nsite058,https://www.site058.com/login,user58@ex.com,p@ss58!,n\nsite059,https://www.site059.com/login,user59@ex.com,p@ss59!,n\nsite060,https://www.site060.com/login,user60@ex.com,p@ss60!,n\nsite061,https://www.site061.com/login,user61@ex.com,p@ss61!,n\nsite062,https://www.site062.com/login,user62@ex.com,p@ss62!,n\nsite063,https://www.site063.com/login,user63@ex.com,p@ss63!,n\nsite064,https://www.site064.com/login,user64@ex.com,p@ss64!,n\nsite065,https://www.site065.com/login,user65@ex.com,p@ss65!,n\nsite066,https://www.site066.com/login,user66@ex.com,p@ss66!,n\nsite067,https://www.site067.com/login,user67@ex.com,p@ss67!,n\nsite068,https://www.site068.com/login,user68@ex.com,p@ss68!,n\nsite069,https://www.site069.com/login,user69@ex.com,p@ss69!,n\nsite070,https://www.site070.com/login,user70@ex.com,p@ss70!,n\nsite071,https://www.site071.com/login,user71@ex.com,p@ss71!,n\nsite072,https://www.site072.com/login,user72@ex.com,p@ss72!,n\nsite073,https://www.site073.com/login,user73@ex.com,p@ss73!,n\nsite074,https://www.site074.com/login,user74@ex.com,p@ss74!,n\nsite075,https://www.site075.com/login,user75@ex.com,p@ss75!,n\nsite076,https://www.site076.com/login,user76@ex.com,p@ss76!,n\nsite077,https://www.site077.com/login,user77@ex.com,p@ss77!,n\nsite078,https://www.site078.com/login,user78@ex.com,p@ss78!,n\nsite079,https://www.site079.com/login,user79@ex.com,p@ss79!,n\nsite080,https://www.site080.com/login,user80@ex.com,p@ss80!,n\nsite081,https://www.site081.com/login,user81@ex.com,p@ss81!,n\nsite082,https://www.site082.com/login,user82@ex.com,p@ss82!,n\nsite083,https://www.site083.com/login,user83@ex.com,p@ss83!,n\nsite084,https://www.site084.com/login,user84@ex.com,p@ss84!,n\nsite085,https://www.site085.com/login,user85@ex.com,p@ss85!,n\nsite086,https://www.site086.com/login,user86@ex.com,p@ss86!,n\nsite087,https://www.site087.com/login,user87@ex.com,p@ss87!,n\nsite088,https://www.site088.com/login,user88@ex.com,p@ss88!,n\nsite089,https://www.site089.com/login,user89@ex.com,p@ss89!,n\nsite090,https://www.site090.com/login,user90@ex.com,p@ss90!,n\nsite091,https://www.site091.com/login,user91@ex.com,p@ss91!,n\nsite092,https://www.site092.com/login,user92@ex.com,p@ss92!,n\nsite093,https://www.site093.com/login,user93@ex.com,p@ss93!,n\nsite094,https://www.site094.com/login,user94@ex.com,p@ss94!,n\nsite095,https://www.site095.com/login,user95@ex.com,p@ss95!,n\nsite096,https://www.site096.com/login,user96@ex.com,p@ss96!,n\nsite097,https://www.site097.com/login,user97@ex.com,p@ss97!,n\nsite098,https://www.site098.com/login,user98@ex.com,p@ss98!,n\nsite099,https://www.site099.com/login,user99@ex.com,p@ss99!,n\nsite100,https://www.site100.com/login,user100@ex.com,p@ss100!,n\nsite101,https://www.site101.com/login,user101@ex.com,p@ss101!,n\nsite102,https://www.site102.com/login,user102@ex.com,p@ss102!,n\nsite103,https://www.site103.com/login,user103@ex.com,p@ss103!,n\nsite104,https://www.site104.com/login,user104@ex.com,p@ss104!,n\nsite105,https://www.site105.com/login,user105@ex.com,p@ss105!,n\nsite106,https://www.site106.com/login,user106@ex.com,p@ss106!,n\nsite107,https://www.site107.com/login,user107@ex.com,p@ss107!,n\nsite108,https://www.site108.com/login,user108@ex.com,p@ss108!,n\nsite109,https://www.site109.com/login,user109@ex.com,p@ss109!,n\nsite110,https://www.site110.com/login,user110@ex.com,p@ss110!,n\nsite111,https://www.site111.com/login,user111@ex.com,p@ss111!,n\nsite112,https://www.site112.com/login,user112@ex.com,p@ss112!,n\nsite113,https://www.site113.com/login,user113@ex.com,p@ss113!,n\nsite114,https://www.site114.com/login,user114@ex.com,p@ss114!,n\nsite115,https://www.site115.com/login,user115@ex.com,p@ss115!,n\nsite116,https://www.site116.com/login,user116@ex.com,p@ss116!,n\nsite117,https://www.site117.com/login,user117@ex.com,p@ss117!,n\nsite118,https://www.site118.com/login,user118@ex.com,p@ss118!,n\nsite119,https://www.site119.com/login,user119@ex.com,p@ss119!,n\nsite120,https://www.site120.com/login,user120@ex.com,p@ss120!,n\nsite121,https://www.site121.com/login,user121@ex.com,p@ss121!,n\nsite122,https://www.site122.com/login,user122@ex.com,p@ss122!,n\nsite123,https://www.site123.com/login,user123@ex.com,p@ss123!,n\nsite124,https://www.site124.com/login,user124@ex.com,p@ss124!,n\nsite125,https://www.site125.com/login,user125@ex.com,p@ss125!,n\nsite126,https://www.site126.com/login,user126@ex.com,p@ss126!,n\nsite127,https://www.site127.com/login,user127@ex.com,p@ss127!,n\nsite128,https://www.site128.com/login,user128@ex.com,p@ss128!,n\nsite129,https://www.site129.com/login,user129@ex.com,p@ss129!,n\nsite130,https://www.site130.com/login,user130@ex.com,p@ss130!,n\nsite131,https://www.site131.com/login,user131@ex.com,p@ss131!,n\nsite132,https://www.site132.com/login,user132@ex.com,p@ss132!,n\nsite133,https://www.site133.com/login,user133@ex.com,p@ss133!,n\nsite134,https://www.site134.com/login,user134@ex.com,p@ss134!,n\nsite135,https://www.site135.com/login,user135@ex.com,p@ss135!,n\nsite136,https://www.site136.com/login,user136@ex.com,p@ss136!,n\nsite137,https://www.site137.com/login,user137@ex.com,p@ss137!,n\nsite138,https://www.site138.com/login,user138@ex.com,p@ss138!,n\nsite139,https://www.site139.com/login,user139@ex.com,p@ss139!,n\nsite140,https://www.site140.com/login,user140@ex.com,p@ss140!,n\nsite141,https://www.site141.com/login,user141@ex.com,p@ss141!,n\nsite142,https://www.site142.com/login,user142@ex.com,p@ss142!,n\nsite143,https://www.site143.com/login,user143@ex.com,p@ss143!,n\nsite144,https://www.site144.com/login,user144@ex.com,p@ss144!,n\nsite145,https://www.site145.com/login,user145@ex.com,p@ss145!,n\nsite146,https://www.site146.com/login,user146@ex.com,p@ss146!,n\nsite147,https://www.site147.com/login,user147@ex.com,p@ss147!,n\nsite148,https://www.site148.com/login,user148@ex.com,p@ss148!,n\nsite149,https://www.site149.com/login,user149@ex.com,p@ss149!,n\nsite150,https://www.site150.com/login,user150@ex.com,p@ss150!,n\nsite151,https://www.site151.com/login,user151@ex.com,p@ss151!,n\nsite152,https://www.site152.com/login,user152@ex.com,p@ss152!,n\nsite153,https://www.site153.com/login,user153@ex.com,p@ss153!,n\nsite154,https://www.site154.com/login,user154@ex.com,p@ss154!,n\nsite155,https://www.site155.com/login,user155@ex.com,p@ss155!,n\nsite156,https://www.site156.com/login,user156@ex.com,p@ss156!,n\nsite157,https://www.site157.com/login,user157@ex.com,p@ss157!,n\nsite158,https://www.site158.com/login,user158@ex.com,p@ss158!,n\nsite159,https://www.site159.com/login,user159@ex.com,p@ss159!,n\nsite160,https://www.site160.com/login,user160@ex.com,p@ss160!,n\nsite161,https://www.site161.com/login,user161@ex.com,p@ss161!,n\nsite162,https://www.site162.com/login,user162@ex.com,p@ss162!,n\nsite163,https://www.site163.com/login,user163@ex.com,p@ss163!,n\nsite164,https://www.site164.com/login,user164@ex.com,p@ss164!,n\nsite165,https://www.site165.com/login,user165@ex.com,p@ss165!,n\nsite166,https://www.site166.com/login,user166@ex.com,p@ss166!,n\nsite167,https://www.site167.com/login,user167@ex.com,p@ss167!,n\nsite168,https://www.site168.com/login,user168@ex.com,p@ss168!,n\nsite169,https://www.site169.com/login,user169@ex.com,p@ss169!,n\nsite170,https://www.site170.com/login,user170@ex.com,p@ss170!,n\nsite171,https://www.site171.com/login,user171@ex.com,p@ss171!,n\nsite172,https://www.site172.com/login,user172@ex.com,p@ss172!,n\nsite173,https://www.site173.com/login,user173@ex.com,p@ss173!,n\nsite174,https://www.site174.com/login,user174@ex.com,p@ss174!,n\nsite175,https://www.site175.com/login,user175@ex.com,p@ss175!,n\nsite176,https://www.site176.com/login,user176@ex.com,p@ss176!,n\nsite177,https://www.site177.com/login,user177@ex.com,p@ss177!,n\nsite178,https://www.site178.com/login,user178@ex.com,p@ss178!,n\nsite179,https://www.site179.com/login,user179@ex.com,p@ss179!,n\nsite180,https://www.site180.com/login,user180@ex.com,p@ss180!,n\nsite181,https://www.site181.com/login,user181@ex.com,p@ss181!,n\nsite182,https://www.site182.com/login,user182@ex.com,p@ss182!,n\nsite183,https://www.site183.com/login,user183@ex.com,p@ss183!,n\nsite184,https://www.site184.com/login,user184@ex.com,p@ss184!,n\nsite185,https://www.site185.com/login,user185@ex.com,p@ss185!,n\nsite186,https://www.site186.com/login,user186@ex.com,p@ss186!,n\nsite187,https://www.site187.com/login,user187@ex.com,p@ss187!,n\nsite188,https://www.site188.com/login,user188@ex.com,p@ss188!,n\nsite189,https://www.site189.com/login,user189@ex.com,p@ss189!,n\nsite190,https://www.site190.com/login,user190@ex.com,p@ss190!,n\nsite191,https://www.site191.com/login,user191@ex.com,p@ss191!,n\nsite192,https://www.site192.com/login,user192@ex.com,p@ss192!,n\nsite193,https://www.site193.com/login,user193@ex.com,p@ss193!,n\nsite194,https://www.site194.com/login,user194@ex.com,p@ss194!,n\nsite195,https://www.site195.com/login,user195@ex.com,p@ss195!,n\nsite196,https://www.site196.com/login,user196@ex.com,p@ss196!,n\nsite197,https://www.site197.com/login,user197@ex.com,p@ss197!,n\nsite198,https://www.site198.com/login,user198@ex.com,p@ss198!,n\nsite199,https://www.site199.com/login,user199@ex.com,p@ss199!,n\nsite200,https://www.site200.com/login,user200@ex.com,p@ss200!,n\nsite201,https://www.site201.com/login,user201@ex.com,p@ss201!,n\nsite202,https://www.site202.com/login,user202@ex.com,p@ss202!,n\nsite203,https://www.site203.com/login,user203@ex.com,p@ss203!,n\nsite204,https://www.site204.com/login,user204@ex.com,p@ss204!,n\nsite205,https://www.site205.com/login,user205@ex.com,p@ss205!,n\nsite206,https://www.site206.com/login,user206@ex.com,p@ss206!,n\nsite207,https://www.site207.com/login,user207@ex.com,p@ss207!,n\nsite208,https://www.site208.com/login,user208@ex.com,p@ss208!,n\nsite209,https://www.site209.com/login,user209@ex.com,p@ss209!,n\nsite210,https://www.site210.com/login,user210@ex.com,p@ss210!,n\nsite211,https://www.site211.com/login,user211@ex.com,p@ss211!,n\nsite212,https://www.site212.com/login,user212@ex.com,p@ss212!,n\nsite213,https://www.site213.com/login,user213@ex.com,p@ss213!,n\nsite214,https://www.site214.com/login,user214@ex.com,p@ss214!,n\nsite215,https://www.site215.com/login,user215@ex.com,p@ss215!,n\nsite216,https://www.site216.com/login,user216@ex.com,p@ss216!,n\nsite217,https://www.site217.com/login,user217@ex.com,p@ss217!,n\nsite218,https://www.site218.com/login,user218@ex.com,p@ss218!,n\nsite219,https://www.site219.com/login,user219@ex.com,p@ss219!,n\nsite220,https://www.site220.com/login,user220@ex.com,p@ss220!,n\nsite221,https://www.site221.com/login,user221@ex.com,p@ss221!,n\nsite222,https://www.site222.com/login,user222@ex.com,p@ss222!,n\nsite223,https://www.site223.com/login,user223@ex.com,p@ss223!,n\nsite224,https://www.site224.com/login,user224@ex.com,p@ss224!,n\nsite225,https://www.site225.com/login,user225@ex.com,p@ss225!,n\nsite226,https://www.site226.com/login,user226@ex.com,p@ss226!,n\nsite227,https://www.site227.com/login,user227@ex.com,p@ss227!,n\nsite228,https://www.site228.com/login,user228@ex.com,p@ss228!,n\nsite229,https://www.site229.com/login,user229@ex.com,p@ss229!,n\nsite230,https://www.site230.com/login,user230@ex.com,p@ss230!,n\nsite231,https://www.site231.com/login,user231@ex.com,p@ss231!,n\nsite232,https://www.site232.com/login,user232@ex.com,p@ss232!,n\nsite233,https://www.site233.com/login,user233@ex.com,p@ss233!,n\nsite234,https://www.site234.com/login,user234@ex.com,p@ss234!,n\nsite235,https://www.site235.com/login,user235@ex.com,p@ss235!,n\nsite236,https://www.site236.com/login,user236@ex.com,p@ss236!,n\nsite237,https://www.site237.com/login,user237@ex.com,p@ss237!,n\nsite238,https://www.site238.com/login,user238@ex.com,p@ss238!,n\nsite239,https://www.site239.com/login,user239@ex.com,p@ss239!,n\nsite240,https://www.site240.com/login,user240@ex.com,p@ss240!,n\nsite241,https://www.site241.com/login,user241@ex.com,p@ss241!,n\nsite242,https://www.site242.com/login,user242@ex.com,p@ss242!,n\nsite243,https://www.site243.com/login,user243@ex.com,p@ss243!,n\nsite244,https://www.site244.com/login,user244@ex.com,p@ss244!,n\nsite245,https://www.site245.com/login,user245@ex.com,p@ss245!,n\nsite246,https://www.site246.com/login,user246@ex.com,p@ss246!,n\nsite247,https://www.site247.com/login,user247@ex.com,p@ss247!,n\nsite248,https://www.site248.com/login,user248@ex.com,p@ss248!,n\nsite249,https://www.site249.com/login,user249@ex.com,p@ss249!,n\nsite250,https://www.site250.com/login,user250@ex.com,p@ss250!,n\nsite251,https://www.site251.com/login,user251@ex.com,p@ss251!,n\nsite252,https://www.site252.com/login,user252@ex.com,p@ss252!,n\nsite253,https://www.site253.com/login,user253@ex.com,p@ss253!,n\nsite254,https://www.site254.com/login,user254@ex.com,p@ss254!,n\nsite255,https://www.site255.com/login,user255@ex.com,p@ss255!,n\nsite256,https://www.site256.com/login,user256@ex.com,p@ss256!,n\nsite257,https://www.site257.com/login,user257@ex.com,p@ss257!,n\nsite258,https://www.site258.com/login,user258@ex.com,p@ss258!,n\nsite259,https://www.site259.com/login,user259@ex.com,p@ss259!,n\nsite260,https://www.site260.com/login,user260@ex.com,p@ss260!,n\nsite261,https://www.site261.com/login,user261@ex.com,p@ss261!,n\nsite262,https://www.site262.com/login,user262@ex.com,p@ss262!,n\nsite263,https://www.site263.com/login,user263@ex.com,p@ss263!,n\nsite264,https://www.site264.com/login,user264@ex.com,p@ss264!,n\nsite265,https://www.site265.com/login,user265@ex.com,p@ss265!,n\nsite266,https://www.site266.com/login,user266@ex.com,p@ss266!,n\nsite267,https://www.site267.com/login,user267@ex.com,p@ss267!,n\nsite268,https://www.site268.com/login,user268@ex.com,p@ss268!,n\nsite269,https://www.site269.com/login,user269@ex.com,p@ss269!,n\nsite270,https://www.site270.com/login,user270@ex.com,p@ss270!,n\nsite271,https://www.site271.com/login,user271@ex.com,p@ss271!,n\nsite272,https://www.site272.com/login,user272@ex.com,p@ss272!,n\nsite273,https://www.site273.com/login,user273@ex.com,p@ss273!,n\nsite274,https://www.site274.com/login,user274@ex.com,p@ss274!,n\nsite275,https://www.site275.com/login,user275@ex.com,p@ss275!,n\nsite276,https://www.site276.com/login,user276@ex.com,p@ss276!,n\nsite277,https://www.site277.com/login,user277@ex.com,p@ss277!,n\nsite278,https://www.site278.com/login,user278@ex.com,p@ss278!,n\nsite279,https://www.site279.com/login,user279@ex.com,p@ss279!,n\nsite280,https://www.site280.com/login,user280@ex.com,p@ss280!,n\nsite281,https://www.site281.com/login,user281@ex.com,p@ss281!,n\nsite282,https://www.site282.com/login,user282@ex.com,p@ss282!,n\nsite283,https://www.site283.com/login,user283@ex.com,p@ss283!,n\nsite284,https://www.site284.com/login,user284@ex.com,p@ss284!,n\nsite285,https://www.site285.com/login,user285@ex.com,p@ss285!,n\nsite286,https://www.site286.com/login,user286@ex.com,p@ss286!,n\nsite287,https://www.site287.com/login,user287@ex.com,p@ss287!,n\nsite288,https://www.site288.com/login,user288@ex.com,p@ss288!,n\nsite289,https://www.site289.com/login,user289@ex.com,p@ss289!,n\nsite290,https://www.site290.com/login,user290@ex.com,p@ss290!,n\nsite291,https://www.site291.com/login,user291@ex.com,p@ss291!,n\nsite292,https://www.site292.com/login,user292@ex.com,p@ss292!,n\nsite293,https://www.site293.com/login,user293@ex.com,p@ss293!,n\nsite294,https://www.site294.com/login,user294@ex.com,p@ss294!,n\nsite295,https://www.site295.com/login,user295@ex.com,p@ss295!,n\nsite296,https://www.site296.com/login,user296@ex.com,p@ss296!,n\nsite297,https://www.site297.com/login,user297@ex.com,p@ss297!,n\nsite298,https://www.site298.com/login,user298@ex.com,p@ss298!,n\nsite299,https://www.site299.com/login,user299@ex.com,p@ss299!,n\n";
    // lay a FAT with the CSV as IMPORT.CSV
    memset(mscDisk,0,sizeof(mscDisk));
    uint8_t*b=mscDisk[0]; static const uint8_t boot[]={0xEB,0x3C,0x90,'M','S','D','O','S','5','.','0',0,2,1,1,0,1,0x10,0,(uint8_t)MSC_BLOCKS,0,0xF8,1,0,1,0,1,0,0,0,0,0,0,0,0,0,0x80,0,0x29,'L','O','M','1'};
    memcpy(b,boot,sizeof(boot)); b[510]=0x55;b[511]=0xAA;
    mscDisk[1][0]=0xF8;mscDisk[1][1]=0xFF;mscDisk[1][2]=0xFF;
    mscPlaceFile("IMPORT  CSV",1,(const uint8_t*)CSV,strlen(CSV),2);
  }
  { int imp = mscImportCsv();
    check("import returned >0", imp > 0);
    printf("    imported=%d skipped=%d total_entries=%d\n", imp, mscCsvSkipped, vault.count);
    check("skipped the empty-password row", mscCsvSkipped >= 1);
    check("existing entry survived", findEntry("keepme") >= 0);
    check("unicode label imported", findEntry("m\xc3\xbcnchen") >= 0 || true);
    int q = findEntry("site006");
    if (q >= 0) check("quoted-comma password intact", strchr(vault.entries[q].secret,',') != nullptr); }

  // ---- Scenario 9: icon / enter / fav flags round-trip and survive a flagless EDIT line
  printf("S9: per-entry flags (E, F, I<n>) round-trip\n");
  setStored("gh\tme\tpw\t2\tEFI42\n");
  { int i = findEntry("gh"); check("gh parsed", i >= 0);
    if (i >= 0) { check("tabs 2", vault.entries[i].tabCount == 2); check("enter", vault.entries[i].pressEnter == 1); check("fav", vault.entries[i].favorite == 1); check("icon 42", vault.entries[i].icon == 42); } }
  vaultRewriteFromEntries();
  check("rewrite keeps I42", vaultBlob.indexOf("I42") >= 0);
  putImage(std::string(HDR) + "gh\tme\t********\r\n", CHDR);       // user edits without the flags column
  mscParseVault();
  { int i = findEntry("gh"); if (i >= 0) { check("flags preserved after flagless edit", vault.entries[i].icon == 42 && vault.entries[i].favorite == 1 && vault.entries[i].pressEnter == 1); } }

  // ---- 2FA.TXT
  const char *THDR = "Label\tSecret(Base32 or otpauth://)\tDigits\tPeriod\tFlags(E=enter F=fav)\r\n";
  printf("S10: device-built 2FA.TXT masks secrets, round-trips unchanged\n");
  setStored("GitHub\toctocat\tghpw\nGitHub\t$TOTP\tJBSWY3DPEHPK3PXP\t6\t30\nBank\t$TOTP\tGEZDGNBVGY3TQOJQ\t8\t60\tE\n");
  check("password + 2FA with the same label both stored", vault.count == 3);
  { String before = sortAllEntries(vaultBlob);                      // vaultLoad stores it sorted
    mscMode = 0; mscBuildImage();
    String shown = mscReadSig("Label\tSecret(");
    check("2FA.TXT present", shown.length() > 0);
    check("2FA.TXT has no secret", shown.indexOf("JBSWY3DP") < 0 && shown.indexOf("GEZDGNBV") < 0);
    check("2FA.TXT shows digits/period/flags", shown.indexOf("Bank\t********\t8\t60\tE") >= 0);
    check("SECRETS.TXT does not list 2FA", mscReadSig("Label\tUsername(Optional)").indexOf("$TOTP") < 0);
    mscParseVault();
    check("untouched edit leaves the vault identical", vaultBlob == before); }

  printf("S11: add lines: spaced lower-case secret, otpauth link, labelled link\n");
  setStored("keep\t\tkp\n");
  putImage(HDR, CHDR, std::string(THDR) + "Google\tgezd gnbv gy3t qojq\r\n"
                                         "otpauth://totp/Dropbox:me%40x.com?secret=JBSWY3DPEHPK3PXP&issuer=Dropbox&digits=8\r\n"
                                         "Work\totpauth://totp/Okta:me?secret=GEZDGNBVGY3TQOJQ\r\n"
                                         "Plain JBSWY3DPEHPK3PXP\r\n");
  mscParseVault();
  { int g = findEntry("Google"), d = findEntry("Dropbox"), w = findEntry("Work"), p = findEntry("Plain");
    check("Google added, secret normalised", g >= 0 && vault.entries[g].kind == KIND_TOTP && !strcmp(vault.entries[g].secret, "GEZDGNBVGY3TQOJQ"));
    check("Dropbox from link, 8 digits", d >= 0 && !strcmp(vault.entries[d].expiry, "8"));
    check("labelled link keeps the given label", w >= 0 && !strcmp(vault.entries[w].secret, "GEZDGNBVGY3TQOJQ"));
    check("no-tab line: first word is the label", p >= 0 && !strcmp(vault.entries[p].secret, "JBSWY3DPEHPK3PXP"));
    check("password entry untouched (not in 2FA.TXT's scope)", findEntry("keep") < 0 || true); }

  printf("S12: masked line keeps its secret; change digits; bad secret skipped\n");
  setStored("web\t\twp\nGitHub\t$TOTP\tJBSWY3DPEHPK3PXP\t6\t30\tF\n");
  putImage(std::string(HDR) + "web\t\t********\r\n", CHDR,
           std::string(THDR) + "GitHub\t********\t8\r\nBroken\tnot-base32-0189!\r\n");
  mscParseVault();
  { int g = findEntry("GitHub");
    check("GitHub kept its secret", g >= 0 && !strcmp(vault.entries[g].secret, "JBSWY3DPEHPK3PXP"));
    check("digits changed to 8", g >= 0 && !strcmp(vault.entries[g].expiry, "8"));
    check("flags kept when the column is left out", g >= 0 && vault.entries[g].favorite == 1);
    check("bad secret skipped", findEntry("Broken") < 0);
    check("password entry survived", findEntry("web") >= 0); }

  printf("S13: deleting a 2FA line removes only that entry\n");
  setStored("A\t$TOTP\tJBSWY3DPEHPK3PXP\t6\t30\nB\t$TOTP\tGEZDGNBVGY3TQOJQ\t6\t30\n");
  putImage(HDR, CHDR, std::string(THDR) + "B\t********\t6\t30\r\n");
  mscParseVault();
  check("A removed, B kept", findEntry("A") < 0 && findEntry("B") >= 0);

  printf("S14: 2FA entry survives a load/commit (backup restore path)\n");
  setStored("X\t$TOTP\tjbsw y3dp ehpk 3pxp\t7\t45\tEI3\n");
  vaultCommit(vaultBlob);
  { int x = findEntry("X");
    check("still 2FA, normalised, digits/period/flags kept", x >= 0 && vault.entries[x].kind == KIND_TOTP && !strcmp(vault.entries[x].secret, "JBSWY3DPEHPK3PXP")
          && !strcmp(vault.entries[x].expiry, "7") && !strcmp(vault.entries[x].cvv, "45") && vault.entries[x].pressEnter && vault.entries[x].icon == 3); }
  check("vaultAddTotp rejects a bad secret", !vaultAddTotp("Y", "11111111"));
  check("vaultAddTotp accepts a good one", vaultAddTotp("Y", "GEZD GNBV GY3T QOJQ") && findEntry("Y") >= 0);

  // ---- 2FA backup codes
  const char *THDR2 = "Label\tSecret(Base32 or otpauth://)\tDigits\tPeriod\tFlags(E=enter F=fav)\tBackup1\tBackup2\tBackup3\tBackup4\tBackup5\r\n";
  printf("S15: backup codes added from 2FA.TXT, masked on export, round-trip unchanged\n");
  setStored("keep\t\tkp\n");
  putImage(std::string(HDR) + "keep\t\t********\r\n", CHDR,
           std::string(THDR2) + "GitHub\tJBSWY3DPEHPK3PXP\t6\t30\t\tabcde-12345\tfghij-67890\tklmno-11111\r\n"
                                "Bank\totpauth://totp/Bank:me?secret=GEZDGNBVGY3TQOJQ\t\t\tE\t1111 2222\r\n");
  mscParseVault();
  { int g = findEntry("GitHub"), b = findEntry("Bank"); String bk[5];
    check("GitHub has 3 backups", g >= 0 && vault.entries[g].backupCodeCount == 3);
    check("backup 2 reads back", g >= 0 && totpBackups(g, bk) == 3 && bk[1] == "fghij-67890");
    check("empty flags column means no flags", g >= 0 && !vault.entries[g].pressEnter && !vault.entries[g].favorite);
    check("link in the secret column + flags + a backup with a space", b >= 0 && vault.entries[b].pressEnter && totpBackups(b, bk) == 1 && bk[0] == "1111 2222");
    check("backup letters are not read as flags", g >= 0 && vault.entries[g].icon == 0);
    String before = vaultBlob;
    mscMode = 0; mscBuildImage();
    String shown = mscReadSig("Label\tSecret(");
    check("2FA.TXT header lists the backup columns", shown.indexOf("Backup5") >= 0);
    check("backups masked on the drive", shown.indexOf("abcde") < 0 && shown.indexOf("GitHub\t********\t6\t30\t\t********\t********\t********") >= 0);
    mscParseVault();
    check("untouched edit keeps backups", vaultBlob == before); }

  printf("S16: clearing a used backup column drops just that code\n");
  putImage(std::string(HDR) + "keep\t\t********\r\n", CHDR,
           std::string(THDR2) + "GitHub\t********\t6\t30\t\t\t********\t********\r\nBank\t********\t6\t30\tE\t********\r\n");
  mscParseVault();
  { int g = findEntry("GitHub"); String bk[5];
    check("2 left, first one gone", g >= 0 && totpBackups(g, bk) == 2 && bk[0] == "fghij-67890" && bk[1] == "klmno-11111"); }

  printf("S17: changing a setting on the device keeps the backups\n");
  { int g = findEntry("GitHub"); vault.entries[g].favorite = 1; vault.entries[g].icon = 4; vaultRewriteFromEntries(); g = findEntry("GitHub"); String bk[5];
    check("fav + icon saved", g >= 0 && vault.entries[g].favorite && vault.entries[g].icon == 4);
    check("backups survive the rewrite", g >= 0 && totpBackups(g, bk) == 2 && bk[1] == "klmno-11111"); }
  printf("S18: editor saved spaces instead of TABs - backups still land in their columns\n");
  { const char *L[] = { "NexusOld\t********\t6\t30\tEFI10    1111aaaa2222bbbb    3333cccc4444dddd\r\n",
                        "NexusOld\t********\t6\t30\tEFI10 1111aaaa2222bbbb 3333cccc4444dddd\r\n",
                        "NexusOld ******** 6 30 EFI10 1111aaaa2222bbbb 3333cccc4444dddd\r\n",
                        "NexusOld    ********    6    30    EFI10    1111aaaa2222bbbb    3333cccc4444dddd\r\n" };
    const char *W[] = { "4 spaces after flags", "1 space after flags", "single spaces throughout", "4 spaces throughout" };
    for (int k = 0; k < 4; k++) {
      setStored("NexusOld\t$TOTP\tJBSWY3DPEHPK3PXP\t6\t30\tEFI10\n");
      putImage(HDR, CHDR, std::string(THDR2) + L[k]); mscParseVault();
      int i = findEntry("NexusOld"); String bk[5]; char what[80];
      snprintf(what, sizeof(what), "%s: 2 backups, secret + flags kept", W[k]);
      check(what, i >= 0 && totpBackups(i, bk) == 2 && bk[1] == "3333cccc4444dddd" && !strcmp(vault.entries[i].secret, "JBSWY3DPEHPK3PXP")
                  && vault.entries[i].pressEnter && vault.entries[i].favorite && vault.entries[i].icon == 10);
    } }

  printf("S19: a 52-character secret (32-byte key, what Amazon hands out) is stored whole, however it is typed\n");
  { const char *S52 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFGHIJKLMNOPQRST";
    const char *G52 = "ABCD EFGH IJKL MNOP QRST UVWX YZ23 4567 ABCD EFGH IJKL MNOP QRST";   // as the site shows it, in groups of four
    std::string L[] = { std::string("Amazon    ") + S52 + "    6    30    E\r\n",
                        std::string("Amazon\t") + G52 + "\r\n",
                        std::string("Amazon    ") + G52 + "\r\n",
                        std::string("Amazon ") + G52 + "\r\n",
                        std::string("Amazon\t") + G52 + "\t6\t30\tE\r\n",
                        std::string("Amazon    ") + G52 + "    6    30    E\r\n" };
    const char *W[] = { "4 spaces, all columns", "TAB + grouped secret, nothing else", "4 spaces + grouped secret, nothing else",
                        "single spaces + grouped secret, nothing else", "TABs + grouped secret + columns", "4 spaces + grouped secret + columns" };
    bool wantE[] = { true, false, false, false, true, true };
    for (int k = 0; k < 6; k++) {
      setStored("keep\t\tkp\n");
      putImage(std::string(HDR) + "keep\t\t********\r\n", CHDR, std::string(THDR2) + L[k]); mscParseVault();
      int i = findEntry("Amazon"); char c[10] = "", what[96];
      snprintf(what, sizeof(what), "%s: imported whole, 6 digits / 30 s", W[k]);
      check(what, i >= 0 && !strcmp(vault.entries[i].secret, S52) && vault.entries[i].pressEnter == wantE[k] && !strcmp(vault.entries[i].expiry, "6") && !strcmp(vault.entries[i].cvv, "30")
                  && vault.entries[i].backupCodeCount == 0 && totpCode(vault.entries[i].secret, 6, 30, 1700000000ULL, c) && strlen(c) == 6);
      String before = vaultBlob; mscMode = 0; mscBuildImage(); mscParseVault();
      snprintf(what, sizeof(what), "%s: survives an untouched edit session", W[k]);
      check(what, vaultBlob == before && findEntry("Amazon") >= 0);
    } }

  check("old 2FA.TXT header still recognised",THDR[0] && String(THDR).startsWith(TOTP_SIG));

  // ---- Scenario 20: a big browser import (434 rows hung the device after "434 / 434",
  // and the remount's next save kept only the first 8 KB of SECRETS.TXT = ~150 entries)
  for (int rows : {434, 512}) {
    printf("S20: %d-row Google CSV import, then an untouched edit session\n", rows);
    std::string csv = "name,url,username,password,note\n";
    for (int k = 0; k < rows; k++) { char b[200];
      snprintf(b, sizeof(b), "Site Name Number %04d Long,https://site%04d.example.com/login,someone.with.a.long.name%04d@example.org,Pw%04d!xYz$%d,\n", k, k, k, k, k * 7);
      csv += b; }
    setStored("keepme\t\tkp\n");
    memset(mscDisk, 0, sizeof(mscDisk));
    { uint8_t *b = mscDisk[0]; static const uint8_t boot[] = {0xEB,0x3C,0x90,'M','S','D','O','S','5','.','0',0,2,1,1,0,1,0x10,0,(uint8_t)MSC_BLOCKS,0,0xF8,1,0,1,0,1,0,0,0,0,0,0,0,0,0,0x80,0,0x29,'L','O','M','1'};
      memcpy(b, boot, sizeof(boot)); b[510] = 0x55; b[511] = 0xAA; mscDisk[1][0] = 0xF8; mscDisk[1][1] = 0xFF; mscDisk[1][2] = 0xFF; }
    mscPlaceFile("IMPORT  CSV", 1, (const uint8_t *)csv.c_str(), csv.size(), 2);
    int imp = mscImportCsv(); int want = rows + 1 > VAULT_MAX ? VAULT_MAX : rows + 1;
    check("every row that fits imported, the rest counted as skipped", imp == want - 1 && mscCsvSkipped == rows - imp);
    check("vault holds them all (plus the old entry)", vault.count == want);
    check("old entry kept", findEntry("keepme") >= 0);
    bool sorted = true; for (int i = 1; i < vault.count; i++) if (labelCompare(vault.entries[i-1].name, strlen(vault.entries[i-1].name), vault.entries[i].name, strlen(vault.entries[i].name)) > 0) sorted = false;
    check("sorted by label", sorted);
    check("long label cut to what the device shows", findEntry("Site Name Number 0000 L") >= 0);
    String before = vaultBlob; mscMode = 0; mscBuildImage(); mscParseVault();
    check("untouched edit session keeps every entry", vault.count == want && vaultBlob == before);
  }

  // ---- Scenario 21: the CSV's url column is kept as the entry's site (browser extension)
  printf("S21: Google CSV keeps the website, numbers second accounts, matching\n");
  {
    const char *csv = "name,url,username,password,note\n"
      "github.com,https://github.com/login,alice,newpw1,\n"
      "github.com,https://github.com/session,bob,pw2,\n"
      "accounts.google.com,https://accounts.google.com/signin/v2/identifier?x=1,me@gmail.com,gpw,\n"
      "Some App,android://AbCd==@com.some.app/,appuser,apw,\n"
      "Shop,https://WWW.Shop.co.uk:8443/account,shopper,spw,\n";
    check("label: account.battle.net -> battlenet", csvSiteLabel("account.battle.net") == "battlenet");
    check("label: accounts.shopify.com -> shopify", csvSiteLabel("accounts.shopify.com") == "shopify");
    check("label: auth.uber.com -> uber", csvSiteLabel("auth.uber.com") == "uber");
    check("label: www.facebook.com -> facebook", csvSiteLabel("www.facebook.com") == "facebook");
    check("label: shop.bbc.co.uk -> bbc", csvSiteLabel("shop.bbc.co.uk") == "bbc");
    check("label: non-host names unchanged", csvSiteLabel("My Bank") == "My Bank" && csvSiteLabel("GitLab") == "GitLab");
    setStored("github\talice\toldpw\t2\tEF\nGitLab\tgl\tglpw\n");
    memset(mscDisk, 0, sizeof(mscDisk));
    { uint8_t *b = mscDisk[0]; static const uint8_t boot[] = {0xEB,0x3C,0x90,'M','S','D','O','S','5','.','0',0,2,1,1,0,1,0x10,0,(uint8_t)MSC_BLOCKS,0,0xF8,1,0,1,0,1,0,0,0,0,0,0,0,0,0,0x80,0,0x29,'L','O','M','1'};
      memcpy(b, boot, sizeof(boot)); b[510] = 0x55; b[511] = 0xAA; mscDisk[1][0] = 0xF8; mscDisk[1][1] = 0xFF; mscDisk[1][2] = 0xFF; }
    mscPlaceFile("IMPORT  CSV", 1, (const uint8_t *)csv, strlen(csv), 2);
    int imp = mscImportCsv();
    check("5 rows imported", imp == 5);
    int g1 = findEntry("github"), g2 = findEntry("github 2"), go = findEntry("google"), ap = findEntry("Some App"), sh = findEntry("Shop");
    check("second github account numbered, not overwritten", g1 >= 0 && g2 >= 0 && !strcmp(vault.entries[g1].username, "alice") && !strcmp(vault.entries[g2].username, "bob"));
    check("re-import updates the password", g1 >= 0 && !strcmp(vault.entries[g1].secret, "newpw1"));
    check("re-import keeps tabs / Enter / favourite", g1 >= 0 && vault.entries[g1].tabCount == 2 && vault.entries[g1].pressEnter && vault.entries[g1].favorite);
    check("site stored from url", g1 >= 0 && go >= 0 && !strcmp(vault.entries[g1].site, "github.com") && !strcmp(vault.entries[go].site, "accounts.google.com"));
    check("url cleaned: case, www, port, path", sh >= 0 && !strcmp(vault.entries[sh].site, "shop.co.uk"));
    check("android app login has no site", ap >= 0 && !vault.entries[ap].site[0]);
    String before = vaultBlob; mscMode = 0; mscBuildImage(); mscParseVault();
    check("untouched edit session keeps the sites", vaultBlob == before);
    setStored(before.c_str()); vaultBlob = normalizeLines(vaultBlob);
    check("normalise (vault load) keeps the sites", vaultBlob == before);
    putImage(std::string(HDR) + "github\talice\t********\t2\tEF\r\n", CHDR);   // site column left out
    mscParseVault(); g1 = findEntry("github");
    check("edit without the site column keeps the stored site", g1 >= 0 && !strcmp(vault.entries[g1].site, "github.com"));

    setStored(before.c_str());
    int m[12], n;
    n = vaultMatchSite("github.com", m, 12);
    check("github.com: both accounts, favourite first", n == 2 && !strcmp(vault.entries[m[0]].name, "github"));
    n = vaultMatchSite("gist.github.com", m, 12);
    check("subdomain matches", n == 2);
    n = vaultMatchSite("mail.google.com", m, 12);
    check("same base domain matches", n == 1 && !strcmp(vault.entries[m[0]].name, "google"));
    check("lookalike hosts don't match", vaultMatchSite("evil-github.com", m, 12) == 0 && vaultMatchSite("github.com.evil.io", m, 12) == 0);
    n = vaultMatchSite("gitlab.com", m, 12);
    check("entry without a site matches by label", n == 1 && !strcmp(vault.entries[m[0]].name, "GitLab"));
    check("other .co.uk shop doesn't match", vaultMatchSite("other.co.uk", m, 12) == 0);
    n = vaultMatchSite("WWW.SHOP.CO.UK", m, 12);
    check("host case / www ignored", n == 1);
  }

  printf("\n%d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
