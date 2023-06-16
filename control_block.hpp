#pragma once

#include <iostream>
#include <vector>
#include <functional>
#include <chrono>
#include <typeinfo>

#include "asio.hpp"
#include "boost/bind/bind.hpp"
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"

#include "cbp_base.hpp"

using namespace std::chrono_literals;

namespace cbp
{
  const short multicast_port = 30001;

  class control_block
  {
  public:
    control_block(asio::io_context &io_context,
                  const asio::ip::address &listen_address,
                  const asio::ip::address &multicast_address)
        : listen_socket_(io_context),
          multicast_socket_(io_context),
          multicast_endpoint_(multicast_address, multicast_port),
          timer_(io_context),
          block_id_(boost::uuids::random_generator()()),
          dispatcher_(to_idx(packet_header::packet_type::number),
                      std::vector<std::function<void()>>(number_of_control_block_states,
                                                         std::bind(&control_block::stub, this)))
    {
      // Create the socket so that multiple may be bound to the same address.
      asio::ip::udp::endpoint listen_endpoint(
          listen_address, multicast_port);
      listen_socket_.open(listen_endpoint.protocol());
      listen_socket_.bind(listen_endpoint);

      // Join the multicast group.
      listen_socket_.set_option(
          asio::ip::multicast::join_group(multicast_address));

      // Set correct packet handlers (i.e. replace stubs as needed)
      dispatcher_[to_idx(packet_header::packet_type::i_am_slave_rsp)][waiting_for_slave] =
          dispatcher_[to_idx(packet_header::packet_type::i_am_slave_rsp)][master] =
              std::bind(&control_block::handle_i_am_slave_response, this);

      dispatcher_[to_idx(packet_header::packet_type::master_needed_req)][waiting_for_slave] =
          dispatcher_[to_idx(packet_header::packet_type::master_needed_req)][master] =
              std::bind(&control_block::handle_master_needed_request, this);

      dispatcher_[to_idx(packet_header::packet_type::slave_needed_req)][waiting_for_slave] =
          dispatcher_[to_idx(packet_header::packet_type::slave_needed_req)][master] =
              std::bind(&control_block::handle_slave_needed_request, this);

      dispatcher_[to_idx(packet_header::packet_type::i_am_master_rsp)][waiting_for_slave] =
          dispatcher_[to_idx(packet_header::packet_type::i_am_master_rsp)][master] =
              std::bind(&control_block::handle_i_am_master_response, this);

      dispatcher_[to_idx(packet_header::packet_type::get_data_rsp)][master] =
          std::bind(&control_block::handle_get_data_response, this);
    }

    virtual void start();

  protected:
    // Two possible states - waiting_for_slave(0) or master(1)
    // They are used for dispatch purposes (state machine)
    enum
    {
      waiting_for_slave,
      master,
      number_of_control_block_states
    };
    int state_ = {waiting_for_slave};
    bool is_waiting_for_slave() { return (state_ == waiting_for_slave); }
    bool is_master() { return (state_ == master); }

    void set_waiting_for_slave_state()
    {
      print_old_state();
      state_ = waiting_for_slave;
      print_new_state();
    }

    void set_master_state()
    {
      print_old_state();
      state_ = master;
      print_new_state();
    }

    virtual void print_state()
    {
      static const char *states[] = {"waiting_for_slave", "master"};
      std::cout << states[state_];
    }

    void print_old_state() { print_state(); }

    void print_new_state()
    {
      std::cout << " -> ";
      print_state();
      std::cout << std::endl;
    }

    int attempts_ = {0};
    packet_header::block_mode mode_ = {packet_header::block_mode::master};

    // Functions
    void apply_sensor_data(const sensor_data &sd)
    {
      t_accum_ += sd.temperature;
      b_accum_ += sd.brightness;
      ++count_accum_;
    }

    bool is_packet_valid(size_t bytes_recvd);

    void stub();
    void calculate_average();
    void send_data();

    void handle_receive_from(const asio::error_code &, size_t);
    void handle_send_to(const asio::error_code &);

    void send_slave_needed();
    void handle_send_slave_needed(const asio::error_code &);
    void handle_slave_needed_sent_tmout(const asio::error_code &);

    void handle_getdata_cycle_tmout(const asio::error_code &);
    void handle_send_get_data(const asio::error_code &);

    void handle_get_data_response();
    void handle_i_am_slave_response();
    void handle_i_am_master_response();
    void handle_master_needed_request();
    void handle_slave_needed_request();

    // Constants
    static constexpr int attempts_max_slave_needed = 2;
    static constexpr int set_data_cycles = 6;

    static constexpr std::chrono::seconds tmout_slave_needed_sent = 3s;
    static constexpr std::chrono::seconds tmout_get_data_cycle = 5s;

    // Data
    asio::ip::udp::socket listen_socket_;
    asio::ip::udp::socket multicast_socket_;
    asio::ip::udp::endpoint multicast_endpoint_;
    asio::ip::udp::endpoint sender_endpoint_;

    asio::steady_timer timer_;
    boost::uuids::uuid block_id_;

    // 2-d array of functions (state machine)
    std::vector<std::vector<std::function<void()>>> dispatcher_;

    uint8_t recv_buf_[max_packet_len] = {0};
    uint8_t send_buf_[max_packet_len] = {0};

    // master-specific data
    int t_accum_ = {0};
    int b_accum_ = {0};
    int count_accum_ = {0};
    int set_data_cycles_ = {0};
    display_data data_for_slaves_;
  };

} // namespace cbp