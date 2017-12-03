#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>

#include <rte_config.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_arp.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_byteorder.h>
#include <rte_launch.h>

#include <arpa/inet.h>

#include "dpdk_init.h"
#include "routing_table.h"
#include "router.h"

// A route in the given format <IP>/<CIDR>,<MAC>,<interface>
// must not be longer than 36 characters at max
#define MAC_LEN 6

// Errors
#define ERR_GEN -1
#define ERR_FORMAT -2
#define ERR_MEM -3
#define ERR_ARG_NULL -4

/**********************************
 *  Static structure definitions  *
 **********************************/
typedef struct routing_table_line {
    uint32_t dst_net;
    uint32_t netmask;
    struct routing_table_entry rte;
    struct routing_table_line *nxt;
} routing_table_line_t;

typedef struct intf_config {
    uint8_t intf;
    uint32_t ip_addr;
    struct intf_config *nxt;
} intf_config_t;


/**********************************
 *  Static function declarations  *
 **********************************/
static int parse_install_route(const char *route);
static int install_route(uint32_t ip_addr, uint8_t prefix,
                     uint8_t port, struct ether_addr* mac_addr);
static int parse_intf_dev(const char *def);
static int add_intf_config(uint8_t intf, uint32_t ip_addr);
static int parse_mac(const char *s_mac, struct ether_addr *mac);
static void print_help();

/**********************************
 *       Lists and Fields         *
 **********************************/
intf_config_t *intf_defs = NULL;
routing_table_line_t *routing_table = NULL;

int router_thread(void* arg)
{
    return 1;
}

void start_thread(uint8_t port)
{
}


/*********************************
 *  Static function definitions  *
 *********************************/

/**
 * /brief Parse a single route and add it to the routing table.
 * 
 * This method will parse a route given as command line argument to the router.
 * After checking the format, we add it to the routing table.
 * Routing definition example: 10.0.10.2/32,52:54:00:cb:ee:f4,0
 * 
 * This function is designed to work on command line arguments.
 * In addition, we do not copy the input string to some local buffer.
 * Therefore, (and because we do not have access to strnstr.. because
 * compiliation is done using the C99 standard) we do not check the length of
 * the command line arguments.
 * 
 * \param route a string containing a single route as command line argument.
 * \return 0 if we could parse the route.
 *          Errors: ERR_FORMAT
 */
static int parse_install_route(const char *route)
{
    char *cidr_start = NULL, *mac_start = NULL, *intf_start = NULL, *tmp = NULL;
    uint32_t ip_addr = 0;
    uint8_t cidr = 0, intf_id = 0;
    long ltmp = 0;
    struct ether_addr mac_addr;

    // Missing CIDR
    if((cidr_start = strstr(route, "/")) == NULL)
        return ERR_FORMAT;
    *cidr_start = '\0';
    cidr_start++;

    // Missing MAC
    if((mac_start = 
        strstr(cidr_start, ",")) == NULL) {
            return ERR_FORMAT;
        }
    *mac_start = '\0';
    mac_start++;

    // Missing MAC
    if((intf_start = 
        strstr(cidr_start, ",")) == NULL) {
            return ERR_FORMAT;
        }
    *intf_start = '\0';
    intf_start++;

    // IP address cannot be converted
    if(inet_pton(AF_INET, route, &ip_addr) != 1)
        return ERR_FORMAT;

    ltmp = strtol(cidr_start, &tmp, 10);
    // Invalid CIDR
    if(tmp != mac_start - 1)
        return ERR_FORMAT;
    if(((uint8_t)ltmp) != ltmp)
        return ERR_FORMAT;
    cidr = (uint8_t)ltmp;

    // Parse the MAC address
    if(parse_mac(mac_start, &mac_addr) < 0)
        return ERR_FORMAT;

    ltmp = strtol(intf_start, &tmp, 10);
    // Invalid interface ID
    if(*tmp != '\0')
        return ERR_FORMAT;
    if(((uint8_t)ltmp) != ltmp)
        return ERR_FORMAT;
    intf_id = (uint8_t)ltmp;

    install_route(ip_addr, cidr, intf_id, &mac_addr);

    return 0;
}

/**
 * /brief Add a new route to the routing/forwarding table.
 * 
 * This methods adds a new route to the routing table. It handles the sorting
 * to enable the LPM algorithm. We will copy the mac address to a new struct.
 * 
 * /param dst_net The IP address of the destination.
 * /param prf The CIDR prefix of the destination.
 * /param mac The MAC of the next hop.
 * /param intf The interface where we can reach the next hop.
 * 
 * /return 0 on success.
 *      Errors: ERR_MEM, ERR_GEN
 */
static int install_route(uint32_t dst_net, uint8_t prf,
                            uint8_t intf, struct ether_addr* mac) {
        routing_table_line_t **iterator = &routing_table;
        routing_table_line_t *new_line = NULL;

        // Store the strip away a possible host part from the dst network.
        uint32_t netmask = (1 << (32 - prf)) - 1;
        dst_net &= netmask;

        while(*iterator != NULL && (*iterator)->netmask > netmask)
            *iterator = (*iterator)->nxt;

        if((new_line = malloc(sizeof(routing_table_line_t))) == NULL)
            return ERR_MEM;

        new_line->nxt = *iterator;
        new_line->dst_net = dst_net;
        new_line->netmask = netmask;
        new_line->rte.dst_port = intf;
        memcpy(&new_line->rte.dst_mac, mac, sizeof(struct ether_addr));

        *iterator = new_line;

        return 0;
    }


/**
 * /brief Parse a single interface definition and add it to the interface config.
 * 
 * This method parses a interface definition of the given format:
 *      <intf_id>,<ip_address>
 * After successful parsing, we add a new interface configuration structure
 * to the list of configurations.
 * 
 * This function is designed to work on command line arguments.
 * In addition, we do not copy the input string to some local buffer.
 * Therefore, (and because we do not have access to strnstr.. because
 * compiliation is done using the C99 standard) we do not check the length of
 * the command line arguments.
 * 
 * \param def a string containing a single interface definition.
 * \return 0 if we could parse the interface definition.
 *          Errors: ERR_FORMAT, ERR_MEM, ERR_GEN
 */
static int parse_intf_dev(const char *def)
{
    long ltmp = 0;
    uint8_t intf = 0;
    uint32_t ip_addr = 0;
    char *ip_start = NULL, *tmp = NULL;

    // Get the seperating ','
    if((ip_start = strstr(def, ",")) == NULL)
        return ERR_FORMAT;
    *ip_start = '\0';
    ip_start++;
    
    ltmp = strtol(def, &tmp, 10);
    // Invalid interface ID
    if(*tmp != '\0')
        return ERR_FORMAT;
    if(((uint8_t)ltmp) != ltmp)
        return ERR_FORMAT;
    intf = (uint8_t)ltmp;
    if(inet_pton(AF_INET, ip_start, &ip_addr) != 1)
        return ERR_FORMAT;
    

    return add_intf_config(intf, ip_addr);
}

/**
 * /brief Add a new interface configuration to the list of
 *      interface configurations.
 * 
 *  Adds a new interface configuration for the given interface and ip address
 * to the list of interface configurations.
 * 
 * /param intf ID of the interface this config belongs to.
 * /param ip_addr IPv4 address of the interface.
 * /return 0 on success.
 *      Errors: ERR_MEM
 */
static int add_intf_config(uint8_t intf, uint32_t ip_addr) {
    intf_config_t **iterator = &intf_defs;

    while(*iterator != NULL)
        iterator = &((*iterator)->nxt);

    if((*iterator = malloc(sizeof(intf_config_t))) == NULL)
        return ERR_MEM;

    (*iterator)->ip_addr = ip_addr;
    (*iterator)->intf = intf;
    (*iterator)->nxt = NULL;

    printf("Added interface configuration for interface %d\n", intf);
    return 0;
}

/**
 * /brief Parse a MAC address contained in a string.
 * 
 * Parse the string which contains a MAC address.
 * The parsed address is returned in the buffer.
 * 
 * \param s_mac a MAC address in colon representation.
 * \param mac a buffer we shall put the parsed address in.
 * \return 0 on successs, < 0 else.
 */
static int parse_mac(const char *s_mac, struct ether_addr *mac)
{
    int buf[MAC_LEN];
    int ctr = 0;

    if(MAC_LEN == sscanf(s_mac, "%x:%x:%x:%x:%x:%x",
                        buf, buf + 1, buf + 2, buf + 3, buf + 4, buf + 5)) {
        for(ctr = 0; ctr < 6; ++ctr)
            mac->addr_bytes[ctr] = (uint8_t) buf[ctr];
        return 0;
    }
    else {
        // Invalid MAC
        return -1;
    }
}

/**
 * /brief Parse all command line arguments.
 * 
 * This method parses all command line arguments the router supports.
 * 
 * \param argc the argc argument the system hands over to our main method.
 * \param argv the argv argument the system hands over to our main method.
 * \return 0 if command line parsing was successful.
 *      Errors: ERR_GEN
 */
int parse_args(int argc, char **argv)
{
    int ctr = 1, err = 0;
    for (ctr = 1; ctr < argc && argv[ctr][0] == '-'; ++ctr) {
        switch (argv[ctr][1]) {
        case 'r':
            if(parse_install_route(argv[++ctr]) < 0) {
                printf("Route definition has an illegal format: '%s'\n",
                    argv[ctr]);
                return ERR_GEN;
            }
            break;
        case 'p':
            if((err = parse_intf_dev(argv[++ctr])) < 0) {
                printf("Error: %d\n", err);
                if(err == ERR_GEN)
                    printf("Could not parse the interface configuration because"
                        "of an unknown error!\n");
                if(err == ERR_FORMAT)
                    printf("Interface configuration has an illegal format: '%s'\n",
                        argv[ctr]);
                else if (err == ERR_MEM)
                    printf("Could not add interface specification."
                        "Out of memory!\n");
                return ERR_GEN;
            }
            break;
        case 'h':
            print_help();
            return 0;
        default:
            print_help();
            return ERR_GEN;
        }   
    }
    return 0;
}

/**
 * Print the help message of this router.
 */
static void print_help()
{

}