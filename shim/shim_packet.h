#ifndef _ZEBRA_SHIM_PACKET_H
#define _ZEBRA_SHIM_PACKET_H

#define OSPF_MAX_PACKET_SIZE  65535U   /* includes IP Header size. */

extern void shim_hello_print (struct ospf6_header * oh);
extern int shim_iobuf_size (unsigned int size);
extern int shim_receive (struct thread *thread);
//extern int shim_hello_send (struct stream * s, struct shim_interface * si);
extern void shim_send(struct in6_addr * src, struct in6_addr * dst, 
	              struct shim_interface * si, struct stream * ibuf);
#endif
