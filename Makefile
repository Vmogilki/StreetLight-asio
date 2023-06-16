ASIO_ROOT=/home/vmogilki/asio-1.26.0
BOOST_ROOT=/home/vmogilki/boost_1_81_0
CXX=g++
CXXFLAGS=-std=c++20 -Wall -Wextra -fno-inline -I$(ASIO_ROOT)/include -I$(BOOST_ROOT) -g \
-DASIO_HAS_BOOST_BIND -DASIO_USE_TS_EXECUTOR_AS_DEFAULT

# Uncomment line below to enable ASIO tracing
#CXXFLAGS+=-DASIO_ENABLE_HANDLER_TRACKING

.PHONY: all
all: control_block client_block

control_block: master_block.o control_block.o cbp_base.o
	$(CXX) -o $@ $^ -static -L$(BOOST_ROOT)/stage/lib/ 

client_block: client_block.o control_block.o cbp_base.o
	$(CXX) -o $@ $^ -static -L$(BOOST_ROOT)/stage/lib/

control_block.o: control_block.cpp control_block.hpp cbp_base.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<  

client_block.o: client_block.cpp control_block.hpp cbp_base.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<  

master_block.o: master_block.cpp control_block.hpp cbp_base.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<  

cbp_base.o: cbp_base.cpp cbp_base.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<  

.PHONY: clean
clean:
	@rm -rf client_block control_block *.o
