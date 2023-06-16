#include "cbp_base.hpp"

namespace cbp
{
  bool
  packet_header::is_packet_valid(uint8_t *net_buf, size_t packet_size)
  {
    if (packet_size < sizeof(packet_header))
    {
      // size too small
      return false;
    }

    if (packet_size > max_packet_len)
    {
      // size too big
      return false;
    }

    auto op = packet_header::op_from_netbuf(net_buf);
    if (op >= packet_type::number)
    {
      // wrong op
      return false;
    }

    if (op == packet_header::packet_type::get_data_rsp &&
        packet_size != (sizeof(packet_header) + sizeof(sensor_data)))
    {
      // wrong size of get_data_rsp packet
      return false;
    }

    if (op == packet_header::packet_type::set_data &&
        packet_size != (sizeof(packet_header) + sizeof(display_data)))
    {
      // wrong size of set_data packet
      return false;
    }

    if (packet_header::mode_from_netbuf(net_buf) > block_mode::tmp_master)
    {
      // wrong mode
      return false;
    }

    return true;
  }
} // namespace cbp