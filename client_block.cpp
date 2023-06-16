#include <random>
#include "control_block.hpp"

namespace cbp
{
  class client_block : public control_block
  {
  public:
    client_block(asio::io_context &io_context,
                 const asio::ip::address &listen_address,
                 const asio::ip::address &multicast_address)
        : control_block(io_context, listen_address, multicast_address),
          random_temperature(-45, 45),
          random_brightness(350, 550)
    {
      // Set correct block's state and mode
      state_ = waiting_for_master;
      mode_ = packet_header::block_mode::tmp_master;

      // expand dispatcher 2d-array to cover additional client_block states
      // i.e. d[7][2] -> d[7][4]
      for (auto &d : dispatcher_)
      {
        for (int n = to_idx(number_of_client_block_states) - to_idx(number_of_control_block_states),
                 i = 0;
             i < n; ++i)
        {
          d.push_back(std::bind(&client_block::stub, this));
        }
      }

      // Set correct packet handlers (i.e. replace the stubs as needed)
      dispatcher_[to_idx(packet_header::packet_type::master_needed_req)][waiting_for_master] =
          dispatcher_[to_idx(packet_header::packet_type::master_needed_req)][slave] =
              std::bind(&client_block::handle_master_needed_request_slave, this);

      dispatcher_[to_idx(packet_header::packet_type::i_am_master_rsp)][waiting_for_slave] =
          dispatcher_[to_idx(packet_header::packet_type::i_am_master_rsp)][master] =
              std::bind(&client_block::handle_i_am_master_response_master, this);

      dispatcher_[to_idx(packet_header::packet_type::i_am_master_rsp)][waiting_for_master] =
          std::bind(&client_block::handle_i_am_master_response_slave, this);

      dispatcher_[to_idx(packet_header::packet_type::slave_needed_req)][waiting_for_slave] =
          dispatcher_[to_idx(packet_header::packet_type::slave_needed_req)][master] =
              std::bind(&client_block::handle_slave_needed_request_master, this);

      dispatcher_[to_idx(packet_header::packet_type::slave_needed_req)][waiting_for_master] =
          std::bind(&client_block::handle_slave_needed_request_wm, this);

      dispatcher_[to_idx(packet_header::packet_type::slave_needed_req)][slave] =
          std::bind(&client_block::handle_slave_needed_request_slave, this);

      dispatcher_[to_idx(packet_header::packet_type::get_data_req)][slave] =
          std::bind(&client_block::handle_get_data_request, this);

      dispatcher_[to_idx(packet_header::packet_type::set_data)][slave] =
          std::bind(&client_block::handle_set_data, this);
    }

    void start() override;

  protected:
    // Four possible states - two control_block states:
    // waiting_for_slave(0) or master(1)
    // plus additional
    // waiting_for_master(2) or slave(3)
    enum
    {
      waiting_for_master = waiting_for_slave + 1,
      slave,
      number_of_client_block_states
    };

    bool is_waiting_for_master() { return (state_ == waiting_for_master); }
    bool is_slave() { return (state_ == slave); }

    void set_waiting_for_master_state()
    {
      print_old_state();
      state_ = waiting_for_master;
      print_new_state();
    }

    void set_slave_state()
    {
      print_old_state();
      state_ = slave;
      print_new_state();
    }

    virtual void print_state()
    {
      static const char *states[] = {"waiting_for_slave", "master", "waiting_for_master", "slave"};
      std::cout << states[state_];
    }

    bool read_sensors_data();
    void display_data_from_master(const display_data &);

    void send_master_needed();
    void handle_send_master_needed(const asio::error_code &);
    void handle_master_needed_sent_tmout(const asio::error_code &);

    void handle_slave_needed_request_slave();
    void handle_slave_needed_request_master();
    void handle_slave_needed_request_wm();
    void handle_master_needed_request_slave();
    void handle_no_request_from_master_tmout(const asio::error_code &);

    void handle_i_am_master_response_slave();
    void handle_i_am_master_response_master();

    void handle_get_data_request();
    void handle_set_data();

    // Constants
    static constexpr int attempts_max_master_needed = 3;

    static constexpr std::chrono::seconds tmout_master_needed_sent = 1s;
    static constexpr std::chrono::seconds tmout_no_request_from_master =
        (6 * tmout_get_data_cycle);

    // slave-specific data
    bool oldest_ = {true};
    sensor_data sensors_ = {{0}, {0}};

    boost::uuids::uuid master_block_id_ = {boost::uuids::nil_uuid()};
    packet_header::block_mode master_mode_ = {packet_header::block_mode::master};

    // Sensors data randomizers
    std::random_device rd;
    std::uniform_int_distribution<int16_t> random_temperature;
    std::uniform_int_distribution<uint16_t> random_brightness;
  };

  void
  client_block::start()
  {
    // Iniate communication by sending slave_needed_request into network
    send_master_needed();

    // listen socket only after all packet handlers are set, though we have stub for unexpected data
    listen_socket_.async_receive_from(
        asio::buffer(recv_buf_, sizeof(recv_buf_)), sender_endpoint_,
        boost::bind(&client_block::handle_receive_from, this,
                    asio::placeholders::error,
                    asio::placeholders::bytes_transferred));
  }

  void
  client_block::send_master_needed()
  {
    mode_ = packet_header::block_mode::slave;

    set_waiting_for_master_state();
    oldest_ = true;

    master_block_id_ = boost::uuids::nil_uuid();

    attempts_ = attempts_max_master_needed;

    // Send multicast master_needed message
    packet_header::to_netbuf(send_buf_,
                             packet_header::packet_type::master_needed_req,
                             mode_,
                             block_id_);

    listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header)),
                                 multicast_endpoint_,
                                 boost::bind(&client_block::handle_send_master_needed,
                                             this, asio::placeholders::error));
  }

  void
  client_block::handle_send_master_needed(const asio::error_code &error)
  {
    // Message sent
    if (!error)
    {
      timer_.expires_after(tmout_master_needed_sent);
      timer_.async_wait(boost::bind(&client_block::handle_master_needed_sent_tmout,
                                    this, asio::placeholders::error));
    }

    // TODO: need to check error
  }

  // Timer function. Slave mode. Send master_needed request N times.
  // If no slave with bigger MAC, goto Master mode
  void
  client_block::handle_master_needed_sent_tmout(const asio::error_code &e)
  {
    if (e == asio::error::operation_aborted)
    {
      return;
    }

    if (is_waiting_for_master())
    {
      if (--attempts_)
      {
        // try one more time - multicast master_needed message
        packet_header::to_netbuf(send_buf_,
                                 packet_header::packet_type::master_needed_req,
                                 mode_,
                                 block_id_);

        listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header)),
                                     multicast_endpoint_,
                                     boost::bind(&client_block::handle_send_master_needed,
                                                 this, asio::placeholders::error));
      }
      else if (oldest_)
      {
        // Still waiting for master and no older IBs after N attempts,
        // so I'll be temporary master waiting for slaves
        send_slave_needed();
      }
    }
  }

  // Stub emulating getting data from real sensors into related data structure.
  // Returns true if operation successful (always)
  bool
  client_block::read_sensors_data()
  {
    sensors_.temperature = random_temperature(rd);
    sensors_.brightness = random_brightness(rd);
    return true;
  }

  void
  client_block::display_data_from_master(const display_data &net_data)
  {
    std::cout << "Displayed: Time["
              << net_data.time
              << "] Info["
              << net_data.text
              << "] Temperature["
              << net_data.temperature
              << "] Brightness["
              << ntohs(net_data.brightness) // from net to host
              << "]" << std::endl;
  }

  // Called for CBP_MASTER_NEEDED_REQ when IB in Wait_for_Master or Slave state.
  // Check if sender has greater MAC which potentially makes this IB poor candidate for Master role
  // ph_mn_req_ignore replacement
  void
  client_block::handle_master_needed_request_slave()
  {
    // check if any IB has greater block_id than this IB
    if (packet_header::id_from_netbuf(recv_buf_) > block_id_)
    {
      oldest_ = false;

      std::cout << "Other IB from ip="
                << sender_endpoint_.address()
                << " with id="
                << packet_header::id_from_netbuf(recv_buf_)
                << " greater than this id="
                << block_id_
                << " in state=";

      print_state();

      std::cout << std::endl;
    }
  }

  // Timer function. Slave mode. Send master_needed request after no get_data request
  // within configured interval.
  void
  client_block::handle_no_request_from_master_tmout(const asio::error_code &e)
  {
    if (e == asio::error::operation_aborted)
    {
      return;
    }

    send_master_needed();
  }

  // Called for CBP_I_AM_MASTER_REP when IB in Wait_for_Master state. Go to Slave state and set
  // timer to control Master availability
  // ph_im_rep_process_s replacement
  void
  client_block::handle_i_am_master_response_slave()
  {
    mode_ = packet_header::block_mode::slave;
    set_slave_state();

    master_block_id_ = packet_header::id_from_netbuf(recv_buf_);
    master_mode_ = packet_header::mode_from_netbuf(recv_buf_);

    attempts_ = 0;

    timer_.expires_after(tmout_no_request_from_master);
    timer_.async_wait(boost::bind(&client_block::handle_no_request_from_master_tmout,
                                  this, asio::placeholders::error));

    std::cout << "New master is set, ip="
              << sender_endpoint_.address()
              << " with id="
              << master_block_id_
              << ", mode=";

    packet_header::print_mode(master_mode_);

    std::cout << std::endl;
  }

  // Called for CBP_I_AM_MASTER_REP when IB in Wait_for_Slave or Master state.
  // If packet not from CB, then ignore it (not expected scenario), otherwise
  // go to Slave state and set timer to control Master availability
  // ph_im_rep_process_m replacement
  void
  client_block::handle_i_am_master_response_master()
  {
    if (packet_header::mode_from_netbuf(recv_buf_) == packet_header::block_mode::master)
    {
      handle_i_am_master_response_slave();
    }
  }

  // Called for CBP_SLAVE_NEEDED_REQ when IB in Wait_for_Master state
  // ph_sn_req_process_wm replacement
  void
  client_block::handle_slave_needed_request_wm()
  {
    // No matter if master is temp or not, we go to slave state anyway
    handle_i_am_master_response_slave();

    // Send response with confirmation
    packet_header::to_netbuf(send_buf_,
                             packet_header::packet_type::i_am_slave_rsp,
                             mode_,
                             block_id_);

    listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header)),
                                 sender_endpoint_,
                                 boost::bind(&client_block::handle_send_to, this,
                                             asio::placeholders::error));
  }

  // Called for CBP_SLAVE_NEEDED_REQ when IB in Slave state
  // ph_sn_req_process_s replacement
  void
  client_block::handle_slave_needed_request_slave()
  {
    // We ignore packet, if our current master is not temp, or packet from
    // temp master with lower block_id, otherwise set new master with
    // confirmation to it, i.e. if current master is temp and (new master is
    // not temp or has greater block_id).
    if (master_mode_ == packet_header::block_mode::tmp_master &&
        (packet_header::id_from_netbuf(recv_buf_) > master_block_id_ ||
         packet_header::mode_from_netbuf(recv_buf_) == packet_header::block_mode::master))
    {
      handle_slave_needed_request_wm();
    }
  }

  // Called for CBP_SLAVE_NEEDED_REQ when IB in Master or Waiting for Slave state
  // ph_sn_req_process_m replacement
  void
  client_block::handle_slave_needed_request_master()
  {
    // We ignore packet, if packet from temp master with lower block_id,
    // otherwise go to Slave and set new master with confirmation to it,
    // i.e. if new master is not temp, or temp but it has greater block_id.

    if (packet_header::mode_from_netbuf(recv_buf_) == packet_header::block_mode::master ||
        ((packet_header::mode_from_netbuf(recv_buf_) == packet_header::block_mode::tmp_master) &&
         packet_header::id_from_netbuf(recv_buf_) > block_id_))
    {
      handle_slave_needed_request_wm();
    }
  }

  // Called for CBP_GET_DATA_REQ when IB in Slave state
  // ph_gd_req_process replacement
  void
  client_block::handle_get_data_request()
  {
    // Check master block_id, i.e. the packet from our Master
    if (packet_header::id_from_netbuf(recv_buf_) == master_block_id_)
    {
      // process get_data request ...
      read_sensors_data();

      std::cout << "GET DATA request from ip="
                << sender_endpoint_.address()
                << " with id="
                << master_block_id_
                << ". Current Temperature="
                << sensors_.temperature
                << ", Brightness="
                << sensors_.brightness
                << std::endl;

      // reset no_request_from_master timer
      timer_.expires_after(tmout_no_request_from_master);
      timer_.async_wait(boost::bind(&client_block::handle_no_request_from_master_tmout,
                                    this, asio::placeholders::error));

      // send get_data response to master
      packet_header::to_netbuf(send_buf_,
                               packet_header::packet_type::get_data_rsp,
                               mode_,
                               block_id_);

      sensors_.to_netbuf(send_buf_);

      listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header) + sizeof(sensors_)),
                                   sender_endpoint_,
                                   boost::bind(&client_block::handle_send_to, this,
                                               asio::placeholders::error));
    }
  }

  // Called for CBP_SET_DATA when IB in Slave state
  // ph_sd_process replacement
  void
  client_block::handle_set_data()
  {
    // Check master block_id, i.e. the packet from our Master
    if (packet_header::id_from_netbuf(recv_buf_) == master_block_id_)
    {
      // Display data
      std::cout << "SET DATA from ip="
                << sender_endpoint_.address()
                << " with id="
                << master_block_id_
                << std::endl;

      display_data_from_master(display_data::from_netbuf(recv_buf_));
    }
  }
} // namespace cbp

int main(int argc, char *argv[])
{
  try
  {
    if (argc != 3)
    {
      std::cerr << "Usage: client_block <listen_address> <multicast_address>\n";
      std::cerr << "  For IPv4, try:\n";
      std::cerr << "    client_block 0.0.0.0 239.255.0.1\n";
      std::cerr << "  For IPv6, try:\n";
      std::cerr << "    client_block 0::0 ff31::8000:1234\n";
      return 1;
    }

    asio::io_context io_context;

    cbp::client_block ib(io_context,
                         asio::ip::make_address(argv[1]),
                         asio::ip::make_address(argv[2]));
    ib.start();

    io_context.run();
  }
  catch (std::exception &e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
