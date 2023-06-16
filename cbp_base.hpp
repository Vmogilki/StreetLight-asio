#pragma once

#include <iostream>
#include <algorithm>
#include <cstring>
#include <netinet/in.h>

#include "boost/uuid/uuid.hpp"

// Convert enum class to underlying type for array index usage
template <typename E>
constexpr auto to_idx(E e) noexcept
{
  return static_cast<std::underlying_type_t<E>>(e);
}

namespace cbp
{
  struct alignas(1) packet_header
  {
    uint16_t operation;
    uint16_t mode;
    boost::uuids::uuid block_id;

    enum class packet_type : decltype(operation)
    {
      master_needed_req,
      i_am_master_rsp,
      slave_needed_req,
      i_am_slave_rsp,
      get_data_req,
      get_data_rsp,
      set_data,
      number
    };

    enum class block_mode : decltype(mode)
    {
      master,
      slave,
      tmp_master
    };

    static void
    print_mode(block_mode m)
    {
      static const char *modes[] = {"master", "slave", "tmp_master"};
      std::cout << modes[to_idx(m)];
    }

    static void
    to_netbuf(uint8_t *net_buf, packet_type pt, block_mode bt, const boost::uuids::uuid &id)
    {
      packet_header *p = reinterpret_cast<packet_header *>(net_buf);
      p->operation = htons(static_cast<decltype(p->operation)>(pt));
      p->mode = htons(static_cast<decltype(p->mode)>(bt));
      p->block_id = id;
    }

    static packet_type
    op_from_netbuf(uint8_t *net_buf)
    {
      packet_header *p = reinterpret_cast<packet_header *>(net_buf);
      return static_cast<packet_type>(ntohs(p->operation));
    }

    static block_mode
    mode_from_netbuf(uint8_t *net_buf)
    {
      packet_header *p = reinterpret_cast<packet_header *>(net_buf);
      return static_cast<block_mode>(ntohs(p->mode));
    }

    static const boost::uuids::uuid &
    id_from_netbuf(uint8_t *net_buf)
    {
      packet_header *p = reinterpret_cast<packet_header *>(net_buf);
      return p->block_id;
    }

    static bool
    is_packet_valid(uint8_t *net_buf, size_t packet_size);
  };

  struct alignas(1) sensor_data
  {
    int16_t temperature = {0};
    uint16_t brightness = {0};

    void from_netbuf(uint8_t *net_buf)
    {
      // read data right after packet_header
      sensor_data *d = reinterpret_cast<sensor_data *>(net_buf + sizeof(packet_header));

      temperature = ntohs(d->temperature);
      brightness = ntohs(d->brightness);
    }

    void to_netbuf(uint8_t *net_buf)
    {
      // wright data right after packet_header
      sensor_data *d = reinterpret_cast<sensor_data *>(net_buf + sizeof(packet_header));

      d->temperature = htons(temperature);
      d->brightness = htons(brightness);
    }
  };

  constexpr int display_txt_len = 45; // Text string ended by trailing zero 
  constexpr int temperature_len = 8;  // Format [+/-]TT °C and trailing zero 
  constexpr int display_time_len = 9; // Format HH:MM:SS and trailing zero 

  struct alignas(1) display_data
  {
    uint16_t brightness = {0};

    uint8_t text[display_txt_len] = {0};
    uint8_t temperature[temperature_len] = {0};
    uint8_t time[display_time_len] = {0};

    void
    to_netbuf(uint8_t *net_buf)
    {
      // write data right after packet_header
      display_data *d = reinterpret_cast<display_data *>(net_buf + sizeof(packet_header));

      // Write the whole structure
      std::memcpy(d, this, sizeof(display_data));

      // Correct brightness (from host to net)
      d->brightness = htons(brightness);
    }

    static const display_data &
    from_netbuf(uint8_t *net_buf)
    {
      // data is right after packet_header
      display_data *d = reinterpret_cast<display_data *>(net_buf + sizeof(packet_header));
      return *d;
    }
  };

  constexpr size_t max_packet_len = sizeof(packet_header) +
                                    std::max(sizeof(sensor_data), sizeof(display_data));
} // namespace cbp