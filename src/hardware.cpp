#include "hardware.h"
#include "esp_timer.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
namespace pump {
uint32_t nowMs() { return uint32_t(uint64_t(esp_timer_get_time()) / 1000); }
void Hardware::safeOff() {
  pumpOn_ = false;
  if (pwmInitialized_) {
    if (ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0) != ESP_OK) {
      pwmHealthy_ = false; gpio_reset_pin(gpio_num_t(board::Output));
      gpio_set_level(gpio_num_t(board::Output), 0);
      gpio_set_direction(gpio_num_t(board::Output), GPIO_MODE_OUTPUT);
    }
  } else {
    gpio_set_level(gpio_num_t(board::Output), 0);
    gpio_set_direction(gpio_num_t(board::Output), GPIO_MODE_OUTPUT);
  }
}
bool Hardware::init() {
  safeOff();
  ledc_timer_config_t timer{}; timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.timer_num = LEDC_TIMER_0; timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.freq_hz = board::PumpPwmHz; timer.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&timer) != ESP_OK) return false;
  ledc_channel_config_t channel{}; channel.gpio_num = board::Output;
  channel.speed_mode = LEDC_LOW_SPEED_MODE; channel.channel = LEDC_CHANNEL_0;
  channel.timer_sel = LEDC_TIMER_0; channel.duty = 0; channel.hpoint = 0;
  if (ledc_channel_config(&channel) != ESP_OK) return false;
  pwmInitialized_ = true;
  ESP_LOGI("pump", "PWM GPIO=%d frequency=%lu Hz; 0%% stops pump; verify driver specification before commissioning",
    board::Output, static_cast<unsigned long>(board::PumpPwmHz));
  return true;
}
bool Hardware::setDuty(uint8_t percent) {
  if (!pwmInitialized_ || !pwmHealthy_ || percent < 1 || percent > 100) return false;
  duty_ = percent;
  const uint32_t value = pumpOn_ ? (1024U * percent + 50U) / 100U : 0;
  // All output writes run in the application task; no fade ISR/service is needed.
  if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, value) == ESP_OK &&
      ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) == ESP_OK) return true;
  pwmHealthy_ = false; safeOff(); return false;
}
void Hardware::pump(bool on) {
  if (!on) { safeOff(); return; }
  if (!pwmHealthy_) { safeOff(); return; }
  pumpOn_ = true;
  if (!setDuty(duty_)) { safeOff(); return; }
}
}
