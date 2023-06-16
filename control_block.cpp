#include <iostream>
#include <sstream>
#include <string>
#include "asio.hpp"
#include "boost/bind/bind.hpp"
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"

#include "cbp_base.hpp"
#include "control_block.hpp"

namespace cbp
{
  bool
  control_block::is_packet_valid(size_t bytes_recvd)
  {
    if (!packet_header::is_packet_valid(recv_buf_, bytes_recvd))
    {
      std::cout << "Discarded packet from ip=" 
                << sender_endpoint_.address() 
                << std::endl;
      return false;
    }
    
    if (packet_header::id_from_netbuf(recv_buf_) == block_id_)
    {
      return false;
    }

    return true;    
  }

  void
  control_block::stub()
  {
    std::cout << "Stub, (unexpected) packet from ip=" 
              << sender_endpoint_.address() 
              << " with id="
              << packet_header::id_from_netbuf(recv_buf_)
              << std::endl;
  }

  void
  control_block::start()
  {
    // Iniate communication by sending slave_needed_request into network
    send_slave_needed();

    // listen socket only after all packet handlers are set, though we have stub for unexpected data
    listen_socket_.async_receive_from(
        asio::buffer(recv_buf_, sizeof(recv_buf_)), sender_endpoint_,
        boost::bind(&control_block::handle_receive_from, this,
                    asio::placeholders::error,
                    asio::placeholders::bytes_transferred));    
  }

  void
  control_block::handle_receive_from(const asio::error_code &error,
                                     size_t bytes_recvd)
  {
    if (!error)
    {
      if (is_packet_valid(bytes_recvd))
      {  
        // Process incoming packet, listen after reply
        dispatcher_[to_idx(packet_header::op_from_netbuf(recv_buf_))][state_]();
      }
    }

    if (!error || error == asio::error::message_size)
    {
      listen_socket_.async_receive_from(asio::buffer(recv_buf_, sizeof(recv_buf_)),
                                        sender_endpoint_,
                                        boost::bind(&control_block::handle_receive_from, this,
                                                    asio::placeholders::error,
                                                    asio::placeholders::bytes_transferred));
    }
  }

  void
  control_block::handle_send_to(const asio::error_code &error)
  {
    if (!error)
    {
    }

    // TODO: process possible error
  }

  void
  control_block::send_slave_needed()
  {
    // Change state
    set_waiting_for_slave_state();

    attempts_ = attempts_max_slave_needed;

    // Send multicast slave_needed message
    packet_header::to_netbuf(send_buf_,
                             packet_header::packet_type::slave_needed_req,
                             mode_,
                             block_id_);

    listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header)),
                                 multicast_endpoint_,
                                 boost::bind(&control_block::handle_send_slave_needed, this,
                                             asio::placeholders::error));
  }

  void
  control_block::handle_send_slave_needed(const asio::error_code &error)
  {
    // Message sent
    if (!error)
    {
      timer_.expires_after(tmout_slave_needed_sent);
      timer_.async_wait(boost::bind(&control_block::handle_slave_needed_sent_tmout, 
                        this, asio::placeholders::error));
    }

    // TODO: need to check error
  }

  void
  control_block::handle_send_get_data(const asio::error_code &error)
  {
    // Message sent
    if (!error)
    {

      timer_.expires_after(tmout_get_data_cycle);
      timer_.async_wait(boost::bind(&control_block::handle_getdata_cycle_tmout, 
                        this, asio::placeholders::error));
    }
    // TODO: need to check error
  }

  // Timer function. Master mode. Send slave_needed request N times.
  void
  control_block::handle_slave_needed_sent_tmout(const asio::error_code &e)
  {
    if (e == asio::error::operation_aborted)
    {
      return;
    }

    if (is_waiting_for_slave() && --attempts_)
    {
      // try one more time - multicast master_needed message
      packet_header::to_netbuf(send_buf_,
                               packet_header::packet_type::slave_needed_req,
                               mode_,
                               block_id_);

      listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header)),
                                   multicast_endpoint_,
                                   boost::bind(&control_block::handle_send_slave_needed,
                                               this, asio::placeholders::error));
    }
    // Otherwise do nothing. Wait for master needed reqs
  }

  // Called for CBP_I_AM_SLAVE_REP when IB/CB in Master or Waiting for Slave state
  // ph_is_rep_process replacement
  void
  control_block::handle_i_am_slave_response()
  {
    // First slave appears - go to Master mode and schedule get_data reqs
    if (is_waiting_for_slave())
    {
      // Fix poor design. We need to set correct master mode_, ie for 
      // control_block (base class) - 'master', for others (derived class(es)) - 'tmp_master'
      mode_ = (typeid(*this) == typeid(control_block)) 
              ? packet_header::block_mode::master : packet_header::block_mode::tmp_master; 
      
      set_master_state();

      // Iniate get_data request from getdata cycle timer
      attempts_ = 1;

      // Send set_data after N get_data cycles
      set_data_cycles_ = set_data_cycles;

      // stop current timer and set get_data cycle timer
      timer_.expires_after(tmout_get_data_cycle);
      timer_.async_wait(boost::bind(&control_block::handle_getdata_cycle_tmout, 
                        this, asio::placeholders::error));
    }

    std::cout << "Another slave IB from ip=" 
              << sender_endpoint_.address()
              << " with id="
              << packet_header::id_from_netbuf(recv_buf_)
              << std::endl;
  }

  // Timer function. Master mode. Check if there are responses from slaves in previous cycle. If yes, then
  // calculate average for previous cycle, clean accumulators, send getdata request for next cycle.
  void
  control_block::handle_getdata_cycle_tmout(const asio::error_code &e)
  {
    if (e == asio::error::operation_aborted)
    {
      return;
    }
    
    // Still master/temp master
    if (is_master())
    {
      // At least one response has been received from slave(s), then send another get_data request
      if (attempts_) 
      {
        calculate_average();

        attempts_ = 0;
       
        packet_header::to_netbuf(send_buf_,
                                 packet_header::packet_type::get_data_req,
                                 mode_,
                                 block_id_);

        listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header)),
                                    multicast_endpoint_,
                                    boost::bind(&control_block::handle_send_get_data,
                                                 this, asio::placeholders::error));
        // Send set_data
        if (!--set_data_cycles_)
        {
          send_data();
        }
      }
      else
      {
        // Fix poor design. We need to set correct master mode_, ie for
        // control_block (base class) - 'master', for others (derived class(es)) - 'tmp_master'
        mode_ = (typeid(*this) == typeid(control_block))
                    ? packet_header::block_mode::master
                    : packet_header::block_mode::tmp_master;

        // seems no slaves. goto in Waiting for Slave state and wait for master_needed request
        set_waiting_for_slave_state();
      }
    }
  }

  // Setup display block basing on accumulated sensor data (temperature and brightness)
  // and clean accumulated data for next cycle.
  void
  control_block::calculate_average()
  {
    if (count_accum_)
    {
      data_for_slaves_.brightness = b_accum_ / count_accum_;

      std::snprintf(reinterpret_cast<char *>(data_for_slaves_.temperature),
                    temperature_len, "%+02d °C", int(t_accum_ / count_accum_));

      std::cout << "Average calculated: T="
                << reinterpret_cast<char *>(data_for_slaves_.temperature)
                << ", B="
                << data_for_slaves_.brightness
                << std::endl;
    }

    t_accum_ = b_accum_ = count_accum_ = 0;
  }

  void
  control_block::send_data() 
  {
    auto t = std::time(nullptr);
    std::strftime(reinterpret_cast<char*>(data_for_slaves_.time),
                  display_time_len, "%T", std::localtime(&t));
    std::snprintf(reinterpret_cast<char*>(data_for_slaves_.text),
                  display_txt_len, "Information message for indication block!");

    //send set_data to slaves
    packet_header::to_netbuf(send_buf_,
                            packet_header::packet_type::set_data,
                            mode_,
                            block_id_);

    data_for_slaves_.to_netbuf(send_buf_);


    listen_socket_.async_send_to(asio::buffer(send_buf_, 
                                              sizeof(packet_header) + sizeof(data_for_slaves_)),
                                 multicast_endpoint_,
                                 boost::bind(&control_block::handle_send_to,
                                              this, asio::placeholders::error));
    // Next packet after N cycles
    set_data_cycles_ = set_data_cycles;
  }

  // Called for CBP_MASTER_NEEDED_REQ when IB in Wait_for_Slave or Master state.
  // ph_mn_req_process replacement
  void
  control_block::handle_master_needed_request()
  {
    handle_i_am_slave_response();

    packet_header::to_netbuf(send_buf_,
                             packet_header::packet_type::i_am_master_rsp,
                             mode_,
                             block_id_);

    listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header)),
                                 sender_endpoint_,
                                 boost::bind(&control_block::handle_send_to,
                                                 this, asio::placeholders::error));
  }

  // Called for CBP_GET_DATA_REP when IB in Master state
  // ph_gd_rep_process replacement
  void
  control_block::handle_get_data_response() 
  {
    // get data from packet
    sensor_data data;
    data.from_netbuf(recv_buf_);

    // Store data for average calculation
    apply_sensor_data(data);

    // reflect get_data response for get_data_cycle timer
    ++attempts_;

    std::cout << "GET DATA response from ip="
              << sender_endpoint_.address()
              << " with id="
              << packet_header::id_from_netbuf(recv_buf_)
              << ". Temperature="
              << data.temperature
              << ". Brightness="
              << data.brightness
              << ". Total responses="
              << attempts_
              << std::endl;
  }

  // Called for CBP_SLAVE_NEEDED_REQ when CB in Master or Waiting for Slave state
  // ph_sn_req_process replacement
  void
  control_block::handle_slave_needed_request()
  {
    // We reply to sender with CBP_I_AM_MASTER_REP. If sender is CBP_DT_MASTER_TEMP (i.e. IB),
    // it must go to Slave state, if sender is CBP_DT_MASTER (i.e. CB), the behavior is currently
    // undefined since we cannot have more than one CB in network. In such a case to avoid races 
    // between CBs we may stop this app
    packet_header::to_netbuf(send_buf_,
                             packet_header::packet_type::i_am_master_rsp,
                             mode_,
                             block_id_);

    listen_socket_.async_send_to(asio::buffer(send_buf_, sizeof(packet_header)),
                                 sender_endpoint_,
                                 boost::bind(&control_block::handle_send_to,
                                                 this, asio::placeholders::error));
  }

  // Called for CBP_I_AM_MASTER_REP when CB in Master or Waiting for Slave state
  // ph_im_rep_process replacement
  void
  control_block::handle_i_am_master_response()
  {
    // Packet is expected only from CB. IB in Master state should not send it,
    // but go to Slave instead. So we ignore such packet.
    if (packet_header::mode_from_netbuf(recv_buf_) == packet_header::block_mode::tmp_master) 
    {
      std::cout << "Warning! Unexpected i_am_master_rsp:tmp_master from ip="
                << sender_endpoint_.address()
                << " with id="
                << packet_header::id_from_netbuf(recv_buf_)
                << std::endl;
    }

    std::cout << "Warning! Another CB is detected in network. Unexpected i_am_master_rsp:master from ip="
              << sender_endpoint_.address()
              << "with id="
              << packet_header::id_from_netbuf(recv_buf_)
              << std::endl;
    
    std::cout << "Exitting..." << std::endl;

    std::exit(EXIT_FAILURE);
  }

} // namespace cbp