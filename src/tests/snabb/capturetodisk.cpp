// compile with: gcc -shared -o capturecallback.so -fPIC capturecallback.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>

#include <iostream>
#include <memory>
#include <thread>

#include <boost/thread/thread.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/atomic.hpp>

#include "../../fastnetmon_packet_parser.h"

typedef struct { unsigned char buffer[1600]; unsigned int length; } packet_buffer_t;
typedef std::shared_ptr<packet_buffer_t> packet_buffer_shared_pointer_t;
typedef boost::lockfree::spsc_queue< packet_buffer_shared_pointer_t,  boost::lockfree::capacity<1048576> > my_spsc_queue_t;

my_spsc_queue_t my_spsc_queue;

extern "C" {
/* Called once before processing packets. */
void firehose_start(); /* optional */
}

/* Called once after processing packets. */
void firehose_stop();  /* optional */

void firehose_stop() {

}

/*
 * Process a packet received from a NIC.
 *
 * pciaddr: name of PCI device packet is received from
 * data:    packet payload (ethernet frame)
 * length:  payload length in bytes
 */

inline void firehose_packet(const char *pciaddr, char *data, int length);

/* Intel 82599 "Legacy" receive descriptor format.
 * See Intel 82599 data sheet section 7.1.5.
 * http://www.intel.com/content/dam/www/public/us/en/documents/datasheets/82599-10-gbe-controller-datasheet.pdf
 */
struct firehose_rdesc {
  uint64_t address;
  uint16_t length;
  uint16_t cksum;
  uint8_t status;
  uint8_t errors;
  uint16_t vlan;
} __attribute__((packed));

/* Traverse the hardware receive descriptor ring.
 * Process each packet that is ready.
 * Return the updated ring index.
 */
extern "C" {

int firehose_callback_v1(const char *pciaddr,
                         char **packets,
                         struct firehose_rdesc *rxring,
                         int ring_size,
                         int index) {
  while (rxring[index].status & 1) {
    int next_index = (index + 1) & (ring_size-1);
    __builtin_prefetch(packets[next_index]);
    firehose_packet(pciaddr, packets[index], rxring[index].length);
    rxring[index].status = 0; /* reset descriptor for reuse */
    index = next_index;
  }
  return index;
}

}

uint64_t received_packets = 0;

void* speed_printer(void* ptr) {
    while (1) {
        uint64_t packets_before = received_packets;
    
        sleep(1);
    
        uint64_t packets_after = received_packets;
        uint64_t pps = packets_after - packets_before;
 
        printf("We process: %llu pps\n", (long long)pps);
    }   
}

void* packets_consumer(void* ptr) {
    packet_buffer_shared_pointer_t packet;

    while (my_spsc_queue.pop(packet)) {
        printf("Pop\n");
        __sync_fetch_and_add(&received_packets, 1);

	//packet.reset();
    }
}

void sigproc(int sig) {
    firehose_stop();

    printf("We caught SINGINT and will finish application\n");
    exit(0);
}

extern "C" {

// We will start speed printer
void firehose_start() {
    signal(SIGINT,  sigproc); 

    pthread_t thread;
    pthread_create(&thread, NULL, speed_printer, NULL);

    pthread_t consumer_thread;
    pthread_create(&consumer_thread, NULL, packets_consumer, NULL);

    pthread_detach(thread);
    pthread_detach(consumer_thread);
}

}

void firehose_packet(const char *pciaddr, char *data, int length) {
    // Put packet to the cache
    //struct pfring_pkthdr packet_header;
    //memset(&packet_header, 0, sizeof(packet_header));
    //packet_header.len = length;
    //packet_header.caplen = length;

    // fastnetmon_parse_pkt((u_char*)data, &packet_header, 3, 0, 0);

    /* 
    char print_buffer[512];
    fastnetmon_print_parsed_pkt(print_buffer, 512, (u_char*)data, &packet_header);
    printf("packet: %s\n", print_buffer);
    */
    std::shared_ptr<packet_buffer_t> packet_pointer( new packet_buffer_t );
    packet_pointer->length = length;

    if (length < 1600) {
        memcpy(packet_pointer->buffer, data, length);
    } else {
        printf("So big packet: %d\n", length);
    }

    // Put pointer to the tube!
    while (!my_spsc_queue.push(packet_pointer)); 
    //__sync_fetch_and_add(&received_packets, 1);
    //printf("Got packet with %d bytes.\n", length);
}

