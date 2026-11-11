#pragma once
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::now_playing {

// The plugin: the tile "player" and the card "player" a tap on it opens. Both read the same media player entity and,
// when the tile's `lyrics` entity option is set, the helper sensor that holds the lyrics. docs/FIRMWARE_API.md has the
// life of a tile and a card.
class NowPlaying : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void dump_config() override;
};

}  // namespace esphome::now_playing
