/**
 * This file contains the libpcap routines needed by the web100srv
 * program.  The global headers and variables are defined in the
 * web100srv.h file. This should make it easier to maintain the
 * web100srv program.
 *
 * Richard Carlson 3/10/04
 * rcarlson@internet2.edu
 */

#include "../config.h"

#include <assert.h>

/* local include file contains needed structures */
#include "web100srv.h"
#include "network.h"
#include "logging.h"
#include <net/if.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include "strlutils.h"
#include "utils.h"

static struct iflists {
  char name[8][32];
  u_int16_t speed[32];
} iflist;

static int dumptrace;
static pcap_t *pd;
static pcap_dumper_t *pdump;
static int* mon_pipe;
static int sigj = 0, sigk = 0;
static int ifspeed;

static struct spdpair fwd, rev;

/** Scan through interface device list and get names/speeds of each interface.
 *
 * The speed data can be used to cap the search for the bottleneck link
 * capacity.  The intent is to reduce the impact of interrupt coalescing on
 * the bottleneck link detection algorithm  RAC 7/14/09
 *
 */
void init_iflist(void) {
  /* pcap_addr_t *ifaceAddr; */
  pcap_if_t *alldevs, *dp;
  struct ethtool_cmd ecmd = {0}; /* Keep valgrind happy */
  int fd, cnt, i, err;
  struct ifreq ifr;
  char errbuf[256];

  cnt = 0;
  if (pcap_findalldevs(&alldevs, errbuf) == 0) {
    for (dp = alldevs; dp != NULL; dp = dp->next) {
      memcpy(iflist.name[cnt++], dp->name, strlen(dp->name));
    }
  }
  // Ethernet related speeds alone "specifically" determined.
  // Note that it appears that NDT assumes its an ethernet link. But, if
  // ifspeed is not assigned, a value will be assigned to it in the
  // "calc_linkspeed" method which will still cause all speed-bins to be
  // looked into (i.e correct results expected).
  for (i = 0; i < cnt; i++) {
    if (strncmp((char *) iflist.name[i], "eth", 3) != 0)
      continue;
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, (char *) iflist.name[i], strlen(iflist.name[i]));
    /* strcpy(ifr.ifr_name, iflist.name[i]); */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ecmd.cmd = ETHTOOL_GSET;
    ifr.ifr_data = (caddr_t) & ecmd;
    err = ioctl(fd, SIOCETHTOOL, &ifr);
    if (err == 0) {
      switch (ecmd.speed) {
        case SPEED_10:
          iflist.speed[i] = DATA_RATE_ETHERNET;
          break;
        case SPEED_100:
          iflist.speed[i] = DATA_RATE_FAST_ETHERNET;
          break;
        case SPEED_1000:
          iflist.speed[i] = DATA_RATE_GIGABIT_ETHERNET;
          break;
        case SPEED_10000:
          iflist.speed[i] = DATA_RATE_10G_ETHERNET;
          break;
        default:
          iflist.speed[i] = DATA_RATE_RTT;
      }
    }
  }
  // free list allocated by pcap_findalldevs call
  if (alldevs != NULL)
    pcap_freealldevs(alldevs);

  for (i = 0; iflist.speed[i] > 0; i++)
    log_println(4, "Generated iflist with device=%s and if_speed=%d",
                iflist.name[i], iflist.speed[i]);
}

/**
 * Force the pcap_loop to return, this is safe to call from a signal handler.
 * Note this will break the loop without a packet being received if
 * used from a signal handler due to the EINTR interrupting pcaps 'read'.
 * 
 * This calls pcap_breakloop with the correct capture.
 */
void force_breakloop(){
  if (pd != NULL) {
    pcap_breakloop(pd);
  }
}

/** 
 *  Send the packet-pair speed bins over pipe to parent.
 */
static void send_bins() {
  log_println(
      5,
      "Received SIGUSRx signal terminating data collection loop for pid=%d",
      getpid());
  log_println(4,
              "Sending pkt-pair data back to parent on pipe %d, %d",
              mon_pipe[0], mon_pipe[1]);
  if (get_debuglvl() > 3) {
    if (fwd.family == 4) {
      fprintf(stderr, "fwd.saddr = %x:%d, rev.saddr = %x:%d\n",
              fwd.saddr[0], fwd.sport, rev.saddr[0], rev.sport);
    } else if (fwd.family == 6) {
      char str[136];
      memset(str, 0, 136);
      inet_ntop(AF_INET6, (void *) fwd.saddr, str, sizeof(str));
      fprintf(stderr, "fwd.saddr = %s:%d", str, fwd.sport);
      memset(str, 0, 136);
      inet_ntop(AF_INET6, (void *) rev.saddr, str, sizeof(str));
      fprintf(stderr, ", rev.saddr = %s:%d\n", str, rev.sport);
    } else {
      fprintf(stderr, "check_signal_flags: Unknown IP family (%d)\n",
              fwd.family);
    }
  }
  print_bins(&fwd, mon_pipe);
  usleep(30000); /* wait here 30 msec, for parent to read this data */
  print_bins(&rev, mon_pipe);
  usleep(30000); /* wait here 30 msec, for parent to read this data */
  if (dumptrace == 1)
    pcap_dump_close(pdump);
  
  log_println(6, "Finished reading pkt-pair data from network, process %d "
              "should terminate now", getpid());
}

/**
 * Initialize variables before starting to accumulate data
 * @param cur SpdPair struct instance
 * */
void init_vars(struct spdpair *cur) {
  int i;

  assert(cur);

  memset(cur->saddr, 0, 4);
  memset(cur->daddr, 0, 4);
  cur->sport = 0;
  cur->dport = 0;
  cur->seq = 0;
  cur->ack = 0;
  cur->win = 0;
  cur->sec = 0;
  cur->usec = 0;
  cur->time = 0;
  cur->totalspd = 0;
  cur->totalcount = 0;
  for (i = 0; i < 16; i++)
    cur->links[i] = 0;
}

/**
 *  This routine prints details of data about speed bins. It also writes the
 *  data into a pipe created to pass this speed bin data among processes
 *  @param cur current speed pair
 *  @param monitor_pipe array used to store file descriptors of pipes
 *   */
void print_bins(struct spdpair *cur, int monitor_pipe[2]) {
  int i, total = 0, max = 0, s, index = -1;
  char buff[256];
  int tzoffset = 6;
  FILE * fp;
  int j;

  assert(cur);

  /* the tzoffset value is fixed for CST (6), use 5 for CDT.  The code needs to find the
   * current timezone and use that value here! */
  s = (cur->st_sec - (tzoffset * 3600)) % 86400;
  fp = fopen(get_logfile(), "a");
  log_print(1, "%02d:%02d:%02d.%06u   ", s / 3600, (s % 3600) / 60, s % 60,
            cur->st_usec);

  // Get the ifspeed bin with the biggest counter value.
  //  NDT determines link speed using this
  for (i = 0; i < 16; i++) {
    total += cur->links[i];
    if (cur->links[i] > max) {
      max = cur->links[i];
    }
  }
  if (get_debuglvl() > 2) {
    if (cur->family == 4) {
      if (fp) {
        fprintf(fp, "%u.%u.%u.%u:%d --> ", (cur->saddr[0] & 0xFF),
                ((cur->saddr[0] >> 8) & 0xff), ((cur->saddr[0] >> 16) & 0xff),
                (cur->saddr[0] >> 24), cur->sport);
        fprintf(fp, "%u.%u.%u.%u:%d  ", (cur->daddr[0] & 0xFF),
                ((cur->daddr[0] >> 8) & 0xff), ((cur->daddr[0] >> 16) & 0xff),
                (cur->daddr[0] >> 24), cur->dport);
      }
      log_print(3, "%u.%u.%u.%u:%d --> ", (cur->saddr[0] & 0xFF),
                ((cur->saddr[0] >> 8) & 0xff), ((cur->saddr[0] >> 16) & 0xff),
                (cur->saddr[0] >> 24), cur->sport);
      log_print(3, "%u.%u.%u.%u:%d ", (cur->daddr[0] & 0xFF),
                ((cur->daddr[0] >> 8) & 0xff), ((cur->daddr[0] >> 16) & 0xff),
                (cur->daddr[0] >> 24), cur->dport);
    }
    else {
      char name[200];
      socklen_t len;
      memset(name, 0, 200);
      len = 199;
      inet_ntop(AF_INET6, cur->saddr, name, len);
      if (fp) {
        fprintf(fp, "%s:%d --> ", name, cur->sport);
      }
      log_print(3, "%s:%d --> ", name, cur->sport);
      memset(name, 0, 200);
      len = 199;
      inet_ntop(AF_INET6, cur->daddr, name, len);
      if (fp) {
        fprintf(fp, "%s:%d  ", name, cur->dport);
      }
      log_print(3, "%s:%d  ", name, cur->dport);
    }
  }
  // If max speed is 0, then indicate that no data has been collected
  if (max == 0) {
    log_println(3, "No data Packets collected");
    if (get_debuglvl() > 2) {
      if (fp) {
        fprintf(fp, "\n\tNo packets collected\n");
      }
    }
    for (i = 0; i < 16; i++)
      cur->links[i] = -1;
    index = -1;
  }
  log_println(3, "Collected pkt-pair data max = %d", max);
  if (max == cur->links[8]) {
    max = 0;
    for (i = 0; i < 10; i++) {
      if (max < cur->links[i]) {
        index = i;
        max = cur->links[i];
      }
    }
  }

  // print details of link speed found
  if (fp) {
    if (get_debuglvl() > 2) {
      switch (index) {
        case -1:
          fprintf(fp, "link=System Fault; ");
          break;
        case 0:
          fprintf(fp, "link=RTT; ");
          break;
        case 1:
          fprintf(fp, "link=dial-up; ");
          break;
        case 2:
          fprintf(fp, "link=T1; ");
          break;
        case 3:
          fprintf(fp, "link=Enet; ");
          break;
        case 4:
          fprintf(fp, "link=T3; ");
          break;
        case 5:
          fprintf(fp, "link=FastE; ");
          break;
        case 6:
          fprintf(fp, "link=OC-12; ");
          break;
        case 7:
          fprintf(fp, "link=GigE; ");
          break;
        case 8:
          fprintf(fp, "link=OC-48; ");
          break;
        case 9:
          fprintf(fp, "link=10 GigE; ");
          break;
        case 10:
          fprintf(fp, "retransmission; ");
          break;
        case 11:
          fprintf(fp, "link=unknown; ");
          break;
      }

      // now print values of speeds for various bins found
      fprintf(fp, "packets=%d\n", total);
      fprintf(fp, "Running Average = %0.2f Mbps  ", cur->totalspd2);
      fprintf(fp, "Average speed = %0.2f Mbps\n",
              cur->totalspd / cur->totalcount);
      fprintf(fp, "\tT1=%d (%0.2f%%); ", cur->links[2],
              ((float) cur->links[2] * 100 / total));
      fprintf(fp, "Ethernet=%d (%0.2f%%); ", cur->links[3],
              ((float) cur->links[3] * 100 / total));
      fprintf(fp, "T3=%d (%0.2f%%); ", cur->links[4],
              ((float) cur->links[4] * 100 / total));
      fprintf(fp, "FastEthernet=%d (%0.2f%%);\n", cur->links[5],
              ((float) cur->links[5] * 100 / total));
      fprintf(fp, "OC-12=%d (%0.2f%%); ", cur->links[6],
              ((float) cur->links[6] * 100 / total));
      fprintf(fp, "\tGigabit Ethernet=%d (%0.2f%%); ", cur->links[7],
              ((float) cur->links[7] * 100 / total));
      fprintf(fp, "OC-48=%d (%0.2f%%); ", cur->links[8],
              ((float) cur->links[8] * 100 / total));
      fprintf(fp, "10 Gigabit Enet=%d (%0.2f%%);\n", cur->links[9],
              ((float) cur->links[9] * 100 / total));
      fprintf(fp, "\tRetransmissions=%d (%0.2f%%); ", cur->links[10],
              ((float) cur->links[10] * 100 / total));
      fprintf(fp, "Unknown=%d (%0.2f%%);\n", cur->links[11],
              ((float) cur->links[11] * 100 / total));
    }
    fclose(fp);
  }

  // make speed bin available to other processes
  snprintf(buff,
           sizeof(buff),
           "  %d %d %d %d %d %d %d %d %d %d %d %d %0.2f %d %d %d %d %d %d",
           cur->links[0], cur->links[1], cur->links[2], cur->links[3],
           cur->links[4], cur->links[5], cur->links[6], cur->links[7],
           cur->links[8], cur->links[9], cur->links[10], cur->links[11],
           cur->totalspd2, cur->inc_cnt, cur->dec_cnt, cur->same_cnt,
           cur->timeout, cur->dupack, ifspeed);
  for (j = 0; j < 5; j++) {
    i = write(monitor_pipe[1], buff, strlen(buff));
    if (i == strlen(buff))
      break;
    if ((i == -1) && (errno == EINTR))
      continue;
  }
  log_println(6, "wrote %d bytes: link counters are '%s'", i, buff);
  log_println(
      6,
      "#$#$#$#$ pcap routine says window increases = %d, decreases = %d, "
      "no change = %d",
      cur->inc_cnt, cur->dec_cnt, cur->same_cnt);
}

/**
 * Calculate the values in speed bins data based on data from 2 packets (speed-pair) received.
 * Each speed bin signifies a range of possible throughput values (for example: Faster than dial-up,
 * but not T1). The throughput calculated based on data
 * from the packets is classified into one such bins and the counter for that bin is incremented.
 *
 * @param cur First speed-pair received
 * @param cur2 Second speed-pair received
 * @param portA Expected destination port
 * @param portB Expected source port
 */
void calculate_spd(struct spdpair *cur, struct spdpair *cur2, int portA,
                   int portB) {
  float bits = 0, spd = 0, time = 0;

  assert(cur);
  assert(cur2);

  time = (((cur->sec - cur2->sec) * 1000000) + (cur->usec - cur2->usec));
  /* time = curt->time - cur2->time; */
  // if ports are as anticipated, use sequence number to calculate no of bits
  // exchanged
  if ((cur->dport == portA) || (cur->sport == portB)) {
    if (cur->seq >= cur2->seq)
      bits = (cur->seq - cur2->seq) * 8;
    else
      bits = 0;
    if (time > 200000) {
      cur2->timeout++;
    }
  } else {  // use acknowledgement details to calculate number of bits exchanged
    if (cur->ack > cur2->ack)
      bits = (cur->ack - cur2->ack) * 8;
    else if (cur->ack == cur2->ack)
      cur2->dupack++;
    else
      bits = 0;
    if (cur->win > cur2->win)
      cur2->inc_cnt++;
    if (cur->win == cur2->win)
      cur2->same_cnt++;
    if (cur->win < cur2->win)
      cur2->dec_cnt++;
  }
  // get throughput
  log_println(8, "0BITS=%f, time=%f, SPD=%f", bits, time, spd);
  spd = (bits / time); /* convert to mbits/sec) */
  log_println(8, "1BITS=%f, time=%f, SPD=%f", bits, time, spd);
  // increment speed bin based on throughput range
  if ((spd > 0) && (spd <= 0.01))
    cur2->links[0]++;
  if ((spd > 0.01) && (spd <= 0.064))
    cur2->links[1]++;
  if ((spd > 0.064) && (spd <= 1.5))
    cur2->links[2]++;
  else if ((spd > 1.5) && (spd <= 10))
    cur2->links[3]++;
  else if ((spd > 10) && (spd <= 40))
    cur2->links[4]++;
  else if ((spd > 40) && (spd <= 100))
    cur2->links[5]++;
  else if ((spd > 100) && (spd <= 622))
    cur2->links[6]++;
  else if ((spd > 622) && (spd <= 1000))
    cur2->links[7]++;
  else if ((spd > 1000) && (spd <= 2400))
    cur2->links[8]++;
  else if ((spd > 2400) && (spd <= 10000))
    cur2->links[9]++;
  else if (spd == 0)
    cur2->links[10]++;
  else
    cur2->links[11]++;
  cur2->seq = cur->seq;
  cur2->ack = cur->ack;
  cur2->win = cur->win;
  cur2->time = cur->time;
  cur2->sec = cur->sec;
  cur2->usec = cur->usec;
  log_println(8, "BITS=%f, time=%f, SPD=%f", bits, time, spd);
  if ((time > 10) && (spd > 0)) {
    log_println(8, ">10 : totalcount=%f, spd=%f, cur2->totalcount=%d",
                cur2->totalspd2, spd, cur2->totalcount);
    cur2->totalspd += spd;
    cur2->totalcount++;
    cur2->totalspd2 = (cur2->totalspd2 + spd) / 2;
  }
  // debug
  //
  // else {
  //    log_println(0, "ELSE totalspd2=%f, spd=%f, cur2->totalcount=%d",
  //                cur2->totalspd2, spd, cur2->totalcount);
  // }

  log_println(8, "totalspd2 in the end=%f, spd=%f",  cur2->totalspd2, spd);
}

/**
 * Read packets received from the network interface. Step through the input file and calculate
 * the link speed between each packet pair. Increment the proper link
 * bin by calling function calculate_spd.
 * "print_speed" seems to be a misnomer.
 * For more information on the parameters, see the pcap library/ pcap manual pages
 * @param user PortPair indicating source/destination ports
 * @param h pcap_pkthdr type packet header information
 * @param p u_char that could point to ethernet/TCP header data
 */

void print_speed(u_char *user, const struct pcap_pkthdr *h, const u_char *p) {
  struct ether_header *enet;
  const struct ip *ip = NULL;
  PortPair* pair = (PortPair*) user;
#if defined(AF_INET6)
  const struct ip6_hdr *ip6;
#endif
  const struct tcphdr *tcp;
  struct spdpair current;
  int port2 = pair->port1;
  int port4 = pair->port2;

  assert(user);

  if (dumptrace == 1)
    pcap_dump((u_char *) pdump, h, p);

  if (pd == NULL) {
    fprintf(
        stderr,
        "!#!#!#!# Error, trying to process IF data, but pcap fd closed\n");
    return;
  }

  current.sec = h->ts.tv_sec;
  current.usec = h->ts.tv_usec;
  current.time = (current.sec * 1000000) + current.usec;

  enet = (struct ether_header *) p;
  p += sizeof(struct ether_header);  // move packet pointer past ethernet fields

  ip = (const struct ip *) p;
  if (ip->ip_v == 4) {
    /* This section grabs the IP & TCP header values from an IPv4 packet and loads the various
     * variables with the packet's values.  
     */
    p += (ip->ip_hl) * 4;
    tcp = (const struct tcphdr *) p;
    current.saddr[0] = ip->ip_src.s_addr;
    current.daddr[0] = ip->ip_dst.s_addr;

    current.sport = ntohs(tcp->source);
    current.dport = ntohs(tcp->dest);
    current.seq = ntohl(tcp->seq);
    current.ack = ntohl(tcp->ack_seq);
    current.win = ntohs(tcp->window);

    /* the current structure now has copies of the IP/TCP header values, if this is the
     * first packet, then there is nothing to compare them to, so just finish the initialization
     * step and return.
     */

    if (fwd.seq == 0) {
      log_println(1,
                  "New IPv4 packet trace started -- initializing counters");
      fwd.seq = current.seq;

      fwd.st_sec = current.sec;
      fwd.st_usec = current.usec;
      rev.st_sec = current.sec;
      rev.st_usec = current.usec;
      fwd.dec_cnt = 0;
      fwd.inc_cnt = 0;
      fwd.same_cnt = 0;
      fwd.timeout = 0;
      fwd.dupack = 0;
      rev.dec_cnt = 0;
      rev.inc_cnt = 0;
      rev.same_cnt = 0;
      rev.timeout = 0;
      rev.dupack = 0;
      fwd.family = 4;
      rev.family = 4;
      return;
    }

    /* a new IPv4 packet has been received and it isn't the 1st one, so calculate the bottleneck link
     * capacity based on the times between this packet and the previous one.
     */

    if (fwd.saddr[0] == current.saddr[0]) {
      if (current.dport == port2) {
        calculate_spd(&current, &fwd, port2, port4);
        return;
      }
      if (current.sport == port4) {
        calculate_spd(&current, &fwd, port2, port4);
        return;
      }
    }
    if (rev.saddr[0] == current.saddr[0]) {
      if (current.sport == port2) {
        calculate_spd(&current, &rev, port2, port4);
        return;
      }
      if (current.dport == port4) {
        calculate_spd(&current, &rev, port2, port4);
        return;
      }
    }
  } else { /*  IP header value is not = 4, so must be IPv6 */
#if defined(AF_INET6)
    // This is an IPv6 packet, grab the IP & TCP header values for further
    // use.

    ip6 = (const struct ip6_hdr *)p;

    p += 40;
    tcp = (const struct tcphdr *)p;
    memcpy(current.saddr, (void *) &ip6->ip6_src, 16);
    memcpy(current.daddr, (void *) &ip6->ip6_dst, 16);

    current.sport = ntohs(tcp->source);
    current.dport = ntohs(tcp->dest);
    current.seq = ntohl(tcp->seq);
    current.ack = ntohl(tcp->ack_seq);
    current.win = ntohs(tcp->window);

    // The 1st packet has been received, finish the initialization process
    // and return.

    if (fwd.seq == 0) {
      log_println(1, "New IPv6 packet trace started -- "
                  "initializing counters");
      fwd.seq = current.seq;
      fwd.st_sec = current.sec;
      fwd.st_usec = current.usec;
      rev.st_sec = current.sec;
      rev.st_usec = current.usec;
      fwd.dec_cnt = 0;
      fwd.inc_cnt = 0;
      fwd.same_cnt = 0;
      fwd.timeout = 0;
      fwd.dupack = 0;
      rev.dec_cnt = 0;
      rev.inc_cnt = 0;
      rev.same_cnt = 0;
      rev.timeout = 0;
      rev.dupack = 0;
      fwd.family = 6;
      rev.family = 6;
      return;
    }

    /* A new packet has been recieved, and it's not the 1st.  Use it to calculate the
     * bottleneck link capacity for this flow.
     */

    if ((fwd.saddr[0] == current.saddr[0]) &&
        (fwd.saddr[1] == current.saddr[1]) &&
        (fwd.saddr[2] == current.saddr[2]) &&
        (fwd.saddr[3] == current.saddr[3])) {
      if (current.dport == port2) {
        calculate_spd(&current, &fwd, port2, port4);
        return;
      }
      if (current.sport == port4) {
        calculate_spd(&current, &fwd, port2, port4);
        return;
      }
    }
    if ((rev.saddr[0] == current.saddr[0]) &&
        (rev.saddr[1] == current.saddr[1]) &&
        (rev.saddr[2] == current.saddr[2]) &&
        (rev.saddr[3] == current.saddr[3])) {
      if (current.sport == port2) {
        calculate_spd(&current, &rev, port2, port4);
        return;
      }
      if (current.dport == port4) {
        calculate_spd(&current, &rev, port2, port4);
        return;
      }
    }
#endif
  }

  /* a packet has been received, so it matched the filter, but the src/dst ports are backward for some reason.
   * Need to fix this by reversing the values.
   */

  if (sigk == 0) {
    sigk++;
    log_println(6,
                "Fault: unknown packet received with src/dst port = %d/%d",
                current.sport, current.dport);
  }
  if (sigj == 0) {
    log_println(6, "Ports need to be reversed now port1/port2 = %d/%d",
                pair->port1, pair->port2);
    int tport = pair->port1;
    pair->port1 = pair->port2;
    pair->port2 = tport;
    fwd.st_sec = current.sec;
    fwd.st_usec = current.usec;
    rev.st_sec = current.sec;
    rev.st_usec = current.usec;
    fwd.dec_cnt = 0;
    fwd.inc_cnt = 0;
    fwd.same_cnt = 0;
    fwd.timeout = 0;
    fwd.dupack = 0;
    rev.dec_cnt = 0;
    rev.inc_cnt = 0;
    rev.same_cnt = 0;
    rev.timeout = 0;
    rev.dupack = 0;
    log_println(6,
                "Ports should have been reversed now port1/port2 = %d/%d",
                pair->port1, pair->port2);
    sigj = 1;
  }
}

/**
 * Perform opening and initialization functions needed
 * by the libpcap routines.  The print_speed function above, is passed
 * to the pcap_open_live() function as the pcap_handler.
 * @param srcAddr 	Source address
 * @param sock_addr socket address used to determine client address
 * @param saddrlen  socket address length
 * @param monitor_pipe socket file descriptors used to read/write data (for interprocess communication)
 * @param device devive detail string
 * @param pair PortPair strcuture
 * @param direction string indicating C2S/S2c test
 * @param compress Option indicating whether log files (here, ndttrace) needs to be compressed. Unused here.
 */

void init_pkttrace(I2Addr srcAddr, struct sockaddr *sock_addr,
                   socklen_t saddrlen, int monitor_pipe[2], char *device,
                   PortPair* pair, const char *direction, int compress) {
  char cmdbuf[256], dir[256];
  pcap_handler printer;
  u_char * pcap_userdata = (u_char*) pair;
  struct bpf_program fcode;
  char errbuf[PCAP_ERRBUF_SIZE];
  int cnt, pflag = 0, i;
  char namebuf[200], isoTime[64];
  size_t nameBufLen = 199;
  I2Addr sockAddr = NULL;
  struct sockaddr *src_addr;
  pcap_if_t *alldevs, *dp;
  pcap_addr_t *curAddr;
  int rc;

  char logdir[256];

  cnt = -1; /* read forever, or until end of file */

  /* Store the monitor pipe as a static global for this file
   * so we can stop the trace later */
  mon_pipe = monitor_pipe;
  init_vars(&fwd);
  init_vars(&rev);

  // scan through the interface device list and get the names/speeds of each
  //  if.  The speed data can be used to cap the search for the bottleneck link
  //  capacity.  The intent is to reduce the impact of interrupt coalescing on
  //  the bottleneck link detection algorithm
  //  RAC 7/14/09
  init_iflist();

  sockAddr = I2AddrBySAddr(get_errhandle(), sock_addr, saddrlen, 0, 0);
  sock_addr = I2AddrSAddr(sockAddr, 0);
  src_addr = I2AddrSAddr(srcAddr, 0);

  // Disable, device can be NULL trying to copy "lo" into
  // it will fail. Also fwd/rev still need to be set
  //
  // Pcap will figure out this should be "lo" below anyway
  // TODO move fwd/rev outside of device == NULL check
#if 0
  /* special check for localhost, set device accordingly */
  if (I2SockAddrIsLoopback(sock_addr, saddrlen) > 0)
    // hardcoding device address to 100, as initialised in main()
    strlcpy(device, "lo", 100);
#endif

  if (device == NULL) {
    if (pcap_findalldevs(&alldevs, errbuf) == 0) {
      for (dp = alldevs; dp != NULL; dp = dp->next) {
        for (curAddr = dp->addresses; curAddr != NULL;
             curAddr = curAddr->next) {
          switch (curAddr->addr->sa_family) {
            case AF_INET:
              memset(namebuf, 0, 200);
              inet_ntop(
                  AF_INET,
                  &((struct sockaddr_in *) curAddr->addr)->sin_addr,
                  namebuf, INET_ADDRSTRLEN);
              log_println(3, "IPv4 interface found address=%s",
                          namebuf);
              if (((struct sockaddr_in *) curAddr->addr)->sin_addr.s_addr
                  == ((struct sockaddr_in *) src_addr)->sin_addr.s_addr) {
                log_println(
                    4,
                    "IPv4 address match, setting device to '%s'",
                    dp->name);
                device = dp->name;
                ifspeed = -1;
                for (i = 0; iflist.name[0][i] != '0'; i++) {
                  if (strncmp((char *) iflist.name[i], device, 4)
                      == 0) {
                    ifspeed = iflist.speed[i];
                    break;
                  }
                }

                if (direction[0] == 's') {
                  fwd.saddr[0] =
                      ((struct sockaddr_in *)src_addr)->sin_addr.s_addr;
                  fwd.daddr[0] =
                      ((struct sockaddr_in *)sock_addr)->sin_addr.s_addr;
                  rev.saddr[0] =
                      ((struct sockaddr_in *)sock_addr)->sin_addr.s_addr;
                  rev.daddr[0] =
                      ((struct sockaddr_in *)src_addr)->sin_addr.s_addr;

                  fwd.sport =
                      ntohs(
                          ((struct sockaddr_in *) src_addr)->sin_port);
                  fwd.dport =
                      ntohs(
                          ((struct sockaddr_in *) sock_addr)->sin_port);
                  rev.sport =
                      ntohs(
                          ((struct sockaddr_in *) sock_addr)->sin_port);
                  rev.dport =
                      ntohs(
                          ((struct sockaddr_in *) src_addr)->sin_port);
                } else {
                  rev.saddr[0] =
                      ((struct sockaddr_in *)src_addr)->sin_addr.s_addr;
                  rev.daddr[0] =
                      ((struct sockaddr_in *)sock_addr)->sin_addr.s_addr;
                  fwd.saddr[0] =
                      ((struct sockaddr_in *)sock_addr)->sin_addr.s_addr;
                  fwd.daddr[0] =
                      ((struct sockaddr_in *)src_addr)->sin_addr.s_addr;

                  rev.sport =
                      ntohs(
                          ((struct sockaddr_in *) src_addr)->sin_port);
                  rev.dport =
                      ntohs(
                          ((struct sockaddr_in *) sock_addr)->sin_port);
                  fwd.sport =
                      ntohs(
                          ((struct sockaddr_in *) sock_addr)->sin_port);
                  fwd.dport =
                      ntohs(
                          ((struct sockaddr_in *) src_addr)->sin_port);
                }
                goto endLoop;
              }
              break;
#if defined(AF_INET6)
            case AF_INET6:
              memset(namebuf, 0, 200);
              inet_ntop(AF_INET6,
                        &((struct sockaddr_in6*)(curAddr->addr))->sin6_addr,
                        namebuf, INET6_ADDRSTRLEN);
              /* I2AddrNodeName(srcAddr, namebuf, &nameBufLen); */
              log_println(3, "IPv6 interface found address=%s", namebuf);
              if (memcmp(((struct sockaddr_in6 *)curAddr->addr)->
                            sin6_addr.s6_addr,
                         ((struct sockaddr_in6 *)src_addr)->
                            sin6_addr.s6_addr,
                         16) == 0) {
                log_println(4, "IPv6 address match, setting device to "
                            "'%s'", dp->name);
                device = dp->name;

                struct sockaddr_in6* src_addr6 =
                    (struct sockaddr_in6*)src_addr;
                struct sockaddr_in6* sock_addr6 =
                    (struct sockaddr_in6*)sock_addr;

                if (direction[0] == 's') {
                  memcpy(fwd.saddr, src_addr6->sin6_addr.s6_addr, 16);
                  memcpy(fwd.daddr, sock_addr6->sin6_addr.s6_addr, 16);
                  memcpy(rev.saddr, sock_addr6->sin6_addr.s6_addr, 16);
                  memcpy(rev.daddr, src_addr6->sin6_addr.s6_addr, 16);
                  fwd.sport = ntohs(src_addr6->sin6_port);
                  fwd.dport = ntohs(sock_addr6->sin6_port);
                  rev.sport = ntohs(sock_addr6->sin6_port);
                  rev.dport = ntohs(src_addr6->sin6_port);
                } else {
                  memcpy(rev.saddr, src_addr6->sin6_addr.s6_addr, 16);
                  memcpy(rev.daddr, sock_addr6->sin6_addr.s6_addr, 16);
                  memcpy(fwd.saddr, sock_addr6->sin6_addr.s6_addr, 16);
                  memcpy(fwd.daddr, src_addr6->sin6_addr.s6_addr, 16);
                  rev.sport = ntohs(src_addr6->sin6_port);
                  rev.dport = ntohs(sock_addr6->sin6_port);
                  fwd.sport = ntohs(sock_addr6->sin6_port);
                  fwd.dport = ntohs(src_addr6->sin6_port);
                }
                goto endLoop;
              }
              break;
#endif
            default:
              log_println(4, "Unknown address family=%d found",
                          curAddr->addr->sa_family);
          }
        }
      }
    }
  }
 endLoop:

  /*  device = pcap_lookupdev(errbuf); */
  if (device == NULL) {
    fprintf(stderr, "pcap_lookupdev failed: %s\n", errbuf);
  }

  log_println(1, "Opening network interface '%s' for packet-pair timing",
              device);

  if ((pd = pcap_open_live(device, 68, !pflag, 1000, errbuf)) == NULL) {
    fprintf(stderr, "pcap_open_live failed: %s\n", errbuf);
  }

  log_println(2, "pcap_open_live() returned pointer %p", pd);

  switch(sock_addr->sa_family) {
      case AF_INET:
          inet_ntop(AF_INET, &(((struct sockaddr_in *)sock_addr)->sin_addr),
                  namebuf, nameBufLen);
          break;

#ifdef AF_INET6
      case AF_INET6:
          inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sock_addr)->sin6_addr),
                  namebuf, nameBufLen);
          break;
#endif

      default:
          I2AddrNodeName(sockAddr, namebuf, &nameBufLen);
  }

  memset(cmdbuf, 0, sizeof(cmdbuf));
  snprintf(cmdbuf, sizeof(cmdbuf), "host %s and port %d", namebuf,
           I2AddrPort(sockAddr));

  log_println(1, "installing pkt filter for '%s'", cmdbuf);
  log_println(1, "Initial pkt src data = %p", fwd.saddr);

  if (pcap_compile(pd, &fcode, cmdbuf, 0, 0xFFFFFF00) < 0) {
    fprintf(stderr, "pcap_compile failed %s\n", pcap_geterr(pd));
    return;
  }

  if (pcap_setfilter(pd, &fcode) < 0) {
    fprintf(stderr, "pcap_setfiler failed %s\n", pcap_geterr(pd));
    return;
  }

  if (dumptrace == 1) {
    // Create log file
    snprintf(dir, sizeof(dir), "%s_%s:%d.%s_ndttrace",
             get_ISOtime(isoTime, sizeof(isoTime)), namebuf,
             I2AddrPort(sockAddr), direction);
    create_named_logdir(logdir, sizeof(logdir), dir, 0);
    pdump = pcap_dump_open(pd, logdir);
    fprintf(stderr, "Opening '%s' log fine\n", logdir);
    if (pdump == NULL) {
      fprintf(stderr, "Unable to create trace file '%s'\n", logdir);
      dumptrace = 0;
    }
  }

  printer = (pcap_handler) print_speed;
  if (dumptrace == 0) {
    for (i = 0; i < 5; i++) {
      rc = write(monitor_pipe[1], "Ready", 6);
      if (rc == 6)
        break;
      if ((rc == -1) && (errno == EINTR))
        continue;
    }
  } else {
    for (i = 0; i < 5; i++) {
      rc = write(monitor_pipe[1], dir, strlen(dir));
      if (rc == strlen(dir))
        break;
      if ((rc == -1) && (errno == EINTR))
        continue;
    }
  }

  /* kill process off if parent doesn't send a signal. */
  alarm(60);

  if (pcap_loop(pd, cnt, printer, pcap_userdata) < 0) {
    log_println(5, "pcap_loop exited %s", pcap_geterr(pd));
  }

  /* Send back results to our parent */
  send_bins();

  pcap_close(pd);

  log_println(
      5,
      "Pkt-Pair data collection ended, waiting for signal to terminate "
      "process");
  /*    Temporarily remove these free statements, The memory should be free'd when
   *      the child process terminates, so we don't have a leak.  There might be a bug in
   *      the pcap lib (on-line reference found from 2003) and on 10/14/09 I was seeing
   *      child crashes with a traceback pointing to the freecode() routine inside the pcap-lib
   *      as the culprit.  RAC 10/15/09
   *
   * if (alldevs != NULL)
   *    pcap_freealldevs(alldevs);
   *  if (&fcode != NULL)
   *    pcap_freecode(&fcode);
   */
  free(sockAddr);

  log_println(
      8,
      "Finally Finished reading data from network, process %d should "
      "terminate now", getpid());
}
