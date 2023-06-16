#include "control_block.hpp"

int main(int argc, char *argv[])
{
  try
  {
    if (argc != 3)
    {
      std::cerr << "Usage: control_block <listen_address> <multicast_address>\n";
      std::cerr << "  For IPv4, try:\n";
      std::cerr << "    control_block 0.0.0.0 239.255.0.1\n";
      std::cerr << "  For IPv6, try:\n";
      std::cerr << "    control_block 0::0 ff31::8000:1234\n";
      return 1;
    }

    asio::io_context io_context;

    cbp::control_block cb(io_context,
                          asio::ip::make_address(argv[1]),
                          asio::ip::make_address(argv[2]));
    cb.start();

    io_context.run();
  }
  catch (std::exception &e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
