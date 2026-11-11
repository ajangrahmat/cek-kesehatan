#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

namespace {

constexpr int OLED_SDA = 21;
constexpr int OLED_SCL = 22;
constexpr uint8_t OLED_ADDRESSES[] = {0x3C, 0x3D};
constexpr uint32_t PAGE_DURATION_MS = 3000;

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
uint8_t oledAddress = 0;
uint8_t displayedPage = 0xFF;

bool findOledAddress() {
  for (uint8_t address : OLED_ADDRESSES) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      oledAddress = address;
      return true;
    }
  }
  return false;
}

void drawInfoPage() {
  oled.drawFrame(0, 0, 128, 64);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(7, 13, "OLED 1.3in SH1106");
  oled.drawHLine(7, 18, 114);
  oled.drawStr(7, 31, "I2C address:");
  oled.setCursor(79, 31);
  oled.print("0x");
  oled.print(oledAddress, HEX);
  oled.drawStr(7, 45, "Resolution: 128x64");
  oled.drawStr(7, 58, "Text and border: OK");
}

void drawPixelPage() {
  oled.drawFrame(0, 0, 128, 64);
  oled.drawVLine(64, 1, 62);
  oled.drawHLine(1, 32, 126);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(8, 14, "PIXEL TEST");
  oled.drawStr(71, 14, "LINES");
  oled.drawStr(8, 45, "CORNERS");
  oled.drawStr(71, 45, "SH1106");

  oled.drawPixel(4, 4);
  oled.drawPixel(123, 4);
  oled.drawPixel(4, 59);
  oled.drawPixel(123, 59);
  oled.drawCircle(96, 51, 8, U8G2_DRAW_ALL);
  oled.drawBox(18, 50, 24, 7);
}

void showPage(uint8_t page) {
  oled.clearBuffer();
  if (page == 0) {
    drawInfoPage();
  } else {
    drawPixelPage();
  }
  oled.sendBuffer();
  displayedPage = page;
  Serial.printf("Menampilkan pola OLED %u\n", page + 1);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!findOledAddress()) {
    Serial.println("OLED tidak ditemukan pada alamat I2C 0x3C atau 0x3D.");
    Serial.println("Periksa VCC=3V3, GND, SDA=GPIO21, SCL=GPIO22.");
    while (true) {
      delay(1000);
    }
  }

  oled.setI2CAddress(oledAddress << 1);
  oled.begin();
  Serial.printf("OLED SH1106 ditemukan di 0x%02X\n", oledAddress);
  showPage(0);
}

void loop() {
  const uint8_t page = (millis() / PAGE_DURATION_MS) % 2;
  if (page != displayedPage) {
    showPage(page);
  }
  delay(10);
}
