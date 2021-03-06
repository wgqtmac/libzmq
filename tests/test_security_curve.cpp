/*
    Copyright (c) 2007-2017 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "testutil.hpp"
#include "testutil_security.hpp"
#if defined (ZMQ_HAVE_WINDOWS)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <stdexcept>
#  define close closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

#include "../src/tweetnacl.h"
#include "../src/curve_client_tools.hpp"
#include "../src/random.hpp"

const char large_identity[] = "0123456789012345678901234567890123456789"
                              "0123456789012345678901234567890123456789"
                              "0123456789012345678901234567890123456789"
                              "0123456789012345678901234567890123456789"
                              "0123456789012345678901234567890123456789"
                              "0123456789012345678901234567890123456789"
                              "012345678901234";

#ifdef ZMQ_BUILD_DRAFT_API
// assert_* are macros rather than functions, to allow assertion failures be
// attributed to the causing source code line
#define assert_no_more_monitor_events_with_timeout(monitor, timeout)           \
    {                                                                          \
        int event_count = 0;                                                   \
        int event, err;                                                        \
        while ((event = get_monitor_event_with_timeout ((monitor), &err, NULL, \
                                                        (timeout)))            \
               != -1) {                                                        \
            ++event_count;                                                     \
            fprintf (stderr, "Unexpected event: %x (err = %i)\n", event, err); \
        }                                                                      \
        assert (event_count == 0);                                             \
    }

#endif

static void zap_handler_large_identity (void *ctx)
{
    zap_handler_generic (ctx, zap_ok, large_identity);
}

void expect_new_client_curve_bounce_fail (void *ctx,
                                          char *server_public,
                                          char *client_public,
                                          char *client_secret,
                                          char *my_endpoint,
                                          void *server)
{
    curve_client_data_t curve_client_data = {server_public, client_public,
                                             client_secret};
    expect_new_client_bounce_fail (
      ctx, my_endpoint, server, socket_config_curve_client, &curve_client_data);
}

void test_garbage_key(void *ctx,
                       void *server,
                       void *server_mon,
                       char *my_endpoint,
                       char *server_public,
                       char *client_public,
                       char *client_secret)
{
    expect_new_client_curve_bounce_fail (ctx, server_public, client_public,
                                         client_secret, my_endpoint, server);

#ifdef ZMQ_BUILD_DRAFT_API
    int handshake_failed_encryption_event_count =
      expect_monitor_event_multiple (server_mon,
                                     ZMQ_EVENT_HANDSHAKE_FAILED_ENCRYPTION);

    // handshake_failed_encryption_event_count should be at least two because 
    // expect_bounce_fail involves two exchanges
    // however, with valgrind we see only one event (maybe the next one takes 
    // very long, or does not happen at all because something else takes very 
    // long)

    fprintf (stderr,
             "count of ZMQ_EVENT_HANDSHAKE_FAILED_ENCRYPTION events: %i\n",
             handshake_failed_encryption_event_count);
#endif
}

void test_curve_security_with_valid_credentials (
  void *ctx, char *my_endpoint, void *server, void *server_mon, int timeout)
{
    curve_client_data_t curve_client_data = {
      valid_server_public, valid_client_public, valid_client_secret};
    void *client = create_and_connect_client (
      ctx, my_endpoint, socket_config_curve_client, &curve_client_data);
    bounce (server, client);
    int rc = zmq_close (client);
    assert (rc == 0);

#ifdef ZMQ_BUILD_DRAFT_API
    int event = get_monitor_event_with_timeout (server_mon, NULL, NULL, -1);
    assert (event == ZMQ_EVENT_HANDSHAKE_SUCCEEDED);

    assert_no_more_monitor_events_with_timeout (server_mon, timeout);
#endif
}

void test_curve_security_with_bogus_client_credentials (
  void *ctx, char *my_endpoint, void *server, void *server_mon, int timeout)
{
    //  This must be caught by the ZAP handler
    char bogus_public [41];
    char bogus_secret [41];
    zmq_curve_keypair (bogus_public, bogus_secret);

    expect_new_client_curve_bounce_fail (ctx, valid_server_public, bogus_public,
                                         bogus_secret, my_endpoint, server);

    int event_count = 0;
#ifdef ZMQ_BUILD_DRAFT_API
    // TODO add another event type ZMQ_EVENT_HANDSHAKE_FAILED_AUTH for this case?
    event_count = expect_monitor_event_multiple (
      server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL, EACCES);
    assert (event_count <= 1);
#endif

    // there may be more than one ZAP request due to repeated attempts by the client
    assert (0 == event_count
            || 1 <= zmq_atomic_counter_value (zap_requests_handled));
}

void expect_zmtp_failure (void *client, char *my_endpoint, void *server, void *server_mon)
{
    //  This must be caught by the curve_server class, not passed to ZAP
    int rc = zmq_connect (client, my_endpoint);
    assert (rc == 0);
    expect_bounce_fail (server, client);
    close_zero_linger (client);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_ZMTP);
#endif

    assert (0 == zmq_atomic_counter_value (zap_requests_handled));
}

void test_curve_security_with_null_client_credentials (void *ctx,
                                                       char *my_endpoint,
                                                       void *server,
                                                       void *server_mon)
{
    void *client = zmq_socket (ctx, ZMQ_DEALER);
    assert (client);

    expect_zmtp_failure (client, my_endpoint, server, server_mon);
}

void test_curve_security_with_plain_client_credentials (void *ctx,
                                                        char *my_endpoint,
                                                        void *server,
                                                        void *server_mon)
{
    void *client = zmq_socket (ctx, ZMQ_DEALER);
    assert (client);
    int rc = zmq_setsockopt (client, ZMQ_PLAIN_USERNAME, "admin", 5);
    assert (rc == 0);
    rc = zmq_setsockopt (client, ZMQ_PLAIN_PASSWORD, "password", 8);
    assert (rc == 0);

    expect_zmtp_failure (client, my_endpoint, server, server_mon);
}

int connect_vanilla_socket (char *my_endpoint)
{
    int s;
    struct sockaddr_in ip4addr;

    unsigned short int port;
    int rc = sscanf (my_endpoint, "tcp://127.0.0.1:%hu", &port);
    assert (rc == 1);

    ip4addr.sin_family = AF_INET;
    ip4addr.sin_port = htons (port);
#if defined(ZMQ_HAVE_WINDOWS) && (_WIN32_WINNT < 0x0600)
    ip4addr.sin_addr.s_addr = inet_addr ("127.0.0.1");
#else
    inet_pton (AF_INET, "127.0.0.1", &ip4addr.sin_addr);
#endif

    s = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    rc = connect (s, (struct sockaddr *) &ip4addr, sizeof (ip4addr));
    assert (rc > -1);
    return s;
}

void test_curve_security_unauthenticated_message (char *my_endpoint,
                                                  void *server,
                                                  int timeout)
{
    // Unauthenticated messages from a vanilla socket shouldn't be received
    int s = connect_vanilla_socket(my_endpoint);
    // send anonymous ZMTP/1.0 greeting
    send (s, "\x01\x00", 2, 0);
    // send sneaky message that shouldn't be received
    send (s, "\x08\x00sneaky\0", 9, 0);

    zmq_setsockopt (server, ZMQ_RCVTIMEO, &timeout, sizeof (timeout));
    char *buf = s_recv (server);
    if (buf != NULL) {
        printf ("Received unauthenticated message: %s\n", buf);
        assert (buf == NULL);
    }
    close (s);
}

void send_all (int fd, const char *data, size_t size)
{
    while (size > 0) {
        int res = send (fd, data, size, 0);
        assert (res > 0);
        size -= res;
        data += res;
    }
}

template <size_t N> void send (int fd, const char (&data) [N])
{
    send_all (fd, data, N - 1);
}

void send_greeting(int s)
{
    send (s, "\xff\0\0\0\0\0\0\0\0\x7f"); // signature
    send (s, "\x03\x00"); // version 3.0
    send (s, "CURVE\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"); // mechanism CURVE
    send (s, "\0"); // as-server == false
    send (s, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
}

void test_curve_security_invalid_hello_wrong_length (char *my_endpoint,
                                                     void *server,
                                                     void *server_mon,
                                                     int timeout)
{
    int s = connect_vanilla_socket (my_endpoint);

    // send GREETING
    send_greeting (s);

    // send CURVE HELLO of wrong size
    send(s, "\x04\x05HELLO");

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_ZMTP,
                                   EPROTO);
#endif

    close (s);
}

const size_t hello_length = 200;
const size_t welcome_length = 168;

zmq::curve_client_tools_t make_curve_client_tools ()
{
    uint8_t valid_client_secret_decoded[32];
    uint8_t valid_client_public_decoded[32];

    zmq_z85_decode (valid_client_public_decoded, valid_client_public);
    zmq_z85_decode (valid_client_secret_decoded, valid_client_secret);

    uint8_t valid_server_public_decoded[32];
    zmq_z85_decode (valid_server_public_decoded, valid_server_public);

    return zmq::curve_client_tools_t (valid_client_public_decoded,
                                      valid_client_secret_decoded,
                                      valid_server_public_decoded);
}

#ifndef htonll
uint64_t htonll (uint64_t value)
{
    // The answer is 42
    static const int num = 42;

    // Check the endianness
    if (*reinterpret_cast<const char *> (&num) == num) {
        const uint32_t high_part = htonl (static_cast<uint32_t> (value >> 32));
        const uint32_t low_part =
          htonl (static_cast<uint32_t> (value & 0xFFFFFFFFLL));

        return (static_cast<uint64_t> (low_part) << 32) | high_part;
    } else {
        return value;
    }
}
#endif

template <size_t N> void send_command (int s, char (&command)[N])
{
  if (N < 256) {
    send(s, "\x04");
    char len = (char)N;
    send_all(s, &len, 1);
  } else {
    send(s, "\x06");
    uint64_t len = htonll (N);
    send_all (s, (char*)&len, 8);
  }
  send_all (s, command, N);
}

void test_curve_security_invalid_hello_command_name (char *my_endpoint,
                                                     void *server,
                                                     void *server_mon,
                                                     int timeout)
{
    int s = connect_vanilla_socket (my_endpoint);

    send_greeting (s);

    zmq::curve_client_tools_t tools = make_curve_client_tools ();

    // send CURVE HELLO with a misspelled command name (but otherwise correct)
    char hello[hello_length];
    int rc = tools.produce_hello (hello, 0);
    assert (rc == 0);
    hello[5] = 'X';

    send_command(s, hello);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_ZMTP,
                                   EPROTO);
#endif

    close (s);
}

void test_curve_security_invalid_hello_version (char *my_endpoint,
                                                void *server,
                                                void *server_mon,
                                                int timeout)
{
    int s = connect_vanilla_socket (my_endpoint);

    send_greeting (s);

    zmq::curve_client_tools_t tools = make_curve_client_tools ();

    // send CURVE HELLO with a wrong version number (but otherwise correct)
    char hello[hello_length];
    int rc = tools.produce_hello (hello, 0);
    assert (rc == 0);
    hello[6] = 2;

    send_command (s, hello);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_ZMTP,
                                   EPROTO);
#endif

    close (s);
}

void flush_read(int fd)
{
    int res;
    char buf[256];

    while ((res = recv (fd, buf, 256, 0)) == 256) {
    }
    assert (res != -1);
}

void recv_all(int fd, uint8_t *data, size_t len)
{
  size_t received = 0;
  while (received < len)
  {
    int res = recv(fd, (char*)data, len, 0);
    assert(res > 0);

    data += res;
    received += res;
  }
}

void recv_greeting (int fd)
{
    uint8_t greeting[64];
    recv_all (fd, greeting, 64);
    //  TODO assert anything about the greeting received from the server?
}

int connect_exchange_greeting_and_send_hello (char *my_endpoint,
                                     zmq::curve_client_tools_t &tools)
{
    int s = connect_vanilla_socket (my_endpoint);

    send_greeting (s);
    recv_greeting (s);

    // send valid CURVE HELLO
    char hello[hello_length];
    int rc = tools.produce_hello (hello, 0);
    assert (rc == 0);

    send_command (s, hello);
    return s;
}

void test_curve_security_invalid_initiate_length (char *my_endpoint,
                                                  void *server,
                                                  void *server_mon,
                                                  int timeout)
{
    zmq::curve_client_tools_t tools = make_curve_client_tools ();

    int s = connect_exchange_greeting_and_send_hello (my_endpoint, tools);

    // receive but ignore WELCOME
    flush_read (s);

#ifdef ZMQ_BUILD_DRAFT_API
    int res = get_monitor_event_with_timeout (server_mon, NULL, NULL, timeout);
    assert (res == -1);
#endif

    send(s, "\x04\x08INITIATE");

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_ZMTP,
                                   EPROTO);
#endif

    close (s);
}

int connect_exchange_greeting_and_hello_welcome (
  char *my_endpoint,
  void *server_mon,
  int timeout,
  zmq::curve_client_tools_t &tools)
{
    int s = connect_exchange_greeting_and_send_hello (
      my_endpoint, tools);

    // receive but ignore WELCOME
    uint8_t welcome[welcome_length + 2];
    recv_all (s, welcome, welcome_length + 2);
    
    int res = tools.process_welcome (welcome + 2, welcome_length);
    assert (res == 0);

#ifdef ZMQ_BUILD_DRAFT_API
    res = get_monitor_event_with_timeout (server_mon, NULL, NULL, timeout);
    assert (res == -1);
#endif

    return s;
}

void test_curve_security_invalid_initiate_command_name (char *my_endpoint,
                                                        void *server,
                                                        void *server_mon,
                                                        int timeout)
{
    zmq::curve_client_tools_t tools = make_curve_client_tools ();
    int s = connect_exchange_greeting_and_hello_welcome (
      my_endpoint, server_mon, timeout, tools);

    char initiate [257];
    tools.produce_initiate (initiate, 257, 1, NULL, 0);
    // modify command name
    initiate[5] = 'X';

    send_command (s, initiate);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_ZMTP,
                                   EPROTO);
#endif

    close (s);
}

void test_curve_security_invalid_initiate_command_encrypted_cookie (
  char *my_endpoint, void *server, void *server_mon, int timeout)
{
    zmq::curve_client_tools_t tools = make_curve_client_tools ();
    int s = connect_exchange_greeting_and_hello_welcome (
      my_endpoint, server_mon, timeout, tools);

    char initiate [257];
    tools.produce_initiate (initiate, 257, 1, NULL, 0);
    // make garbage from encrypted cookie
    initiate[30] = !initiate[30];

    send_command (s, initiate);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (
      server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_ENCRYPTION, EPROTO);
#endif

    close (s);
}

void test_curve_security_invalid_initiate_command_encrypted_content (
  char *my_endpoint, void *server, void *server_mon, int timeout)
{
    zmq::curve_client_tools_t tools = make_curve_client_tools ();
    int s = connect_exchange_greeting_and_hello_welcome (
      my_endpoint, server_mon, timeout, tools);

    char initiate [257];
    tools.produce_initiate (initiate, 257, 1, NULL, 0);
    // make garbage from encrypted content
    initiate[150] = !initiate[150];

    send_command (s, initiate);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (
      server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_ENCRYPTION, EPROTO);
#endif

    close (s);
}

void test_curve_security_invalid_keysize (void *ctx)
{
    //  Check return codes for invalid buffer sizes
    void *client = zmq_socket (ctx, ZMQ_DEALER);
    assert (client);
    errno = 0;
    int rc = zmq_setsockopt (client, ZMQ_CURVE_SERVERKEY, valid_server_public, 123);
    assert (rc == -1 && errno == EINVAL);
    errno = 0;
    rc = zmq_setsockopt (client, ZMQ_CURVE_PUBLICKEY, valid_client_public, 123);
    assert (rc == -1 && errno == EINVAL);
    errno = 0;
    rc = zmq_setsockopt (client, ZMQ_CURVE_SECRETKEY, valid_client_secret, 123);
    assert (rc == -1 && errno == EINVAL);
    rc = zmq_close (client);
    assert (rc == 0);
}

int main (void)
{
    if (!zmq_has ("curve")) {
        printf ("CURVE encryption not installed, skipping test\n");
        return 0;
    }

    zmq::random_open ();

    setup_testutil_security_curve ();

    int timeout = 250;

    setup_test_environment ();

    void *ctx;
    void *handler;
    void *zap_thread;
    void *server;
    void *server_mon;
    char my_endpoint [MAX_SOCKET_STRING];

    fprintf (stderr, "test_curve_security_with_valid_credentials\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_with_valid_credentials (ctx, my_endpoint, server,
                                                server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    char garbage_key[] = "0000000000000000000000000000000000000000";

    //  Check CURVE security with a garbage server key
    //  This will be caught by the curve_server class, not passed to ZAP
    fprintf (stderr, "test_garbage_server_key\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_garbage_key (ctx, server, server_mon, my_endpoint, garbage_key,
                      valid_client_public, valid_client_secret);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    //  Check CURVE security with a garbage client public key
    //  This will be caught by the curve_server class, not passed to ZAP
    fprintf (stderr, "test_garbage_client_public_key\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_garbage_key (ctx, server, server_mon, my_endpoint, valid_server_public,
                      garbage_key, valid_client_secret);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    //  Check CURVE security with a garbage client secret key
    //  This will be caught by the curve_server class, not passed to ZAP
    fprintf (stderr, "test_garbage_client_secret_key\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_garbage_key (ctx, server, server_mon, my_endpoint, valid_server_public,
                      valid_client_public, garbage_key);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_with_bogus_client_credentials\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_with_bogus_client_credentials (ctx, my_endpoint, server,
                                                       server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_with_null_client_credentials\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_with_null_client_credentials (ctx, my_endpoint, server,
                                                      server_mon);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_with_plain_client_credentials\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_with_plain_client_credentials (ctx, my_endpoint, server,
                                                       server_mon);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_unauthenticated_message\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_unauthenticated_message (my_endpoint, server, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    //  tests with misbehaving CURVE client
    fprintf (stderr, "test_curve_security_invalid_hello_wrong_length\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_invalid_hello_wrong_length (my_endpoint, server,
                                                    server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);
    fprintf (stderr, "test_curve_security_invalid_hello_command_name\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_invalid_hello_command_name (my_endpoint, server,
                                                    server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_invalid_hello_command_version\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_invalid_hello_version (my_endpoint, server, server_mon,
                                               timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_invalid_initiate_command_length\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_invalid_initiate_length (my_endpoint, server,
                                                 server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_invalid_initiate_command_name\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_invalid_initiate_command_name (my_endpoint, server,
                                                       server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_invalid_initiate_command_encrypted_cookie\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_invalid_initiate_command_encrypted_cookie (
      my_endpoint, server, server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    fprintf (stderr, "test_curve_security_invalid_initiate_command_encrypted_content\n");
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
    test_curve_security_invalid_initiate_command_encrypted_content (
      my_endpoint, server, server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    //  test with a large identity (resulting in large metadata)
    fprintf (stderr, "test_curve_security_with_valid_credentials (large identity)\n");
    setup_context_and_server_side (
      &ctx, &handler, &zap_thread, &server, &server_mon, my_endpoint,
      &zap_handler_large_identity, &socket_config_curve_server, &valid_server_secret,
      large_identity);
    test_curve_security_with_valid_credentials (ctx, my_endpoint, server,
                                                server_mon, timeout);
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
            handler);

    ctx = zmq_ctx_new ();
    test_curve_security_invalid_keysize (ctx);
    int rc = zmq_ctx_term (ctx);
    assert (rc == 0);

    zmq::random_close ();

    return 0;
}
