/*
    Copyright (c) 2007-2016 Contributors as noted in the AUTHORS file

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

#include "precompiled.hpp"

#include <string>

#include "msg.hpp"
#include "session_base.hpp"
#include "err.hpp"
#include "plain_server.hpp"
#include "wire.hpp"

zmq::plain_server_t::plain_server_t (session_base_t *session_,
                                     const std::string &peer_address_,
                                     const options_t &options_) :
    mechanism_t (options_),
    zap_client_common_handshake_t (
      session_, peer_address_, options_, sending_welcome)
{
}

zmq::plain_server_t::~plain_server_t ()
{
}

int zmq::plain_server_t::next_handshake_command (msg_t *msg_)
{
    int rc = 0;

    switch (state) {
        case sending_welcome:
            rc = produce_welcome (msg_);
            if (rc == 0)
                state = waiting_for_initiate;
            break;
        case sending_ready:
            rc = produce_ready (msg_);
            if (rc == 0)
                state = ready;
            break;
        case sending_error:
            rc = produce_error (msg_);
            if (rc == 0)
                state = error_sent;
            break;
        default:
            errno = EAGAIN;
            rc = -1;
    }
    return rc;
}

int zmq::plain_server_t::process_handshake_command (msg_t *msg_)
{
    int rc = 0;

    switch (state) {
        case waiting_for_hello:
            rc = process_hello (msg_);
            break;
        case waiting_for_initiate:
            rc = process_initiate (msg_);
            break;
        default:
            //  Temporary support for security debugging
            puts ("PLAIN I: invalid handshake command");
            errno = EPROTO;
            rc = -1;
            break;
    }
    if (rc == 0) {
        rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
    }
    return rc;
}

int zmq::plain_server_t::process_hello (msg_t *msg_)
{
    const unsigned char *ptr = static_cast <unsigned char *> (msg_->data ());
    size_t bytes_left = msg_->size ();

    if (bytes_left < 6 || memcmp (ptr, "\x05HELLO", 6)) {
        //  Temporary support for security debugging
        puts ("PLAIN I: invalid PLAIN client, did not send HELLO");
        errno = EPROTO;
        return -1;
    }
    ptr += 6;
    bytes_left -= 6;

    if (bytes_left < 1) {
        //  Temporary support for security debugging
        puts ("PLAIN I: invalid PLAIN client, did not send username");
        errno = EPROTO;
        return -1;
    }
    const size_t username_length = static_cast <size_t> (*ptr++);
    bytes_left -= 1;

    if (bytes_left < username_length) {
        //  Temporary support for security debugging
        puts ("PLAIN I: invalid PLAIN client, sent malformed username");
        errno = EPROTO;
        return -1;
    }
    const std::string username = std::string ((char *) ptr, username_length);
    ptr += username_length;
    bytes_left -= username_length;
    if (bytes_left < 1) {
        //  Temporary support for security debugging
        puts ("PLAIN I: invalid PLAIN client, did not send password");
        errno = EPROTO;
        return -1;
    }

    const size_t password_length = static_cast <size_t> (*ptr++);
    bytes_left -= 1;
    if (bytes_left < password_length) {
        //  Temporary support for security debugging
        puts ("PLAIN I: invalid PLAIN client, sent malformed password");
        errno = EPROTO;
        return -1;
    }

    const std::string password = std::string ((char *) ptr, password_length);
    ptr += password_length;
    bytes_left -= password_length;
    if (bytes_left > 0) {
        //  Temporary support for security debugging
        puts ("PLAIN I: invalid PLAIN client, sent extraneous data");
        errno = EPROTO;
        return -1;
    }

    //  Use ZAP protocol (RFC 27) to authenticate the user.
    //  Note that there is no point to PLAIN if ZAP is not set up to handle the
    //  username and password, so if ZAP is not configured it is considered a
    //  failure.
    int rc = session->zap_connect ();
    if (rc != 0)
        return -1;
    send_zap_request (username, password);
    return receive_and_process_zap_reply () == -1 ? -1 : 0;
}

int zmq::plain_server_t::produce_welcome (msg_t *msg_) const
{
    const int rc = msg_->init_size (8);
    errno_assert (rc == 0);
    memcpy (msg_->data (), "\x07WELCOME", 8);
    return 0;
}

int zmq::plain_server_t::process_initiate (msg_t *msg_)
{
    const unsigned char *ptr = static_cast <unsigned char *> (msg_->data ());
    const size_t bytes_left = msg_->size ();

    if (bytes_left < 9 || memcmp (ptr, "\x08INITIATE", 9)) {
        //  Temporary support for security debugging
        puts ("PLAIN I: invalid PLAIN client, did not send INITIATE");
        errno = EPROTO;
        return -1;
    }
    const int rc = parse_metadata (ptr + 9, bytes_left - 9);
    if (rc == 0)
        state = sending_ready;
    return rc;
}

int zmq::plain_server_t::produce_ready (msg_t *msg_) const
{
    make_command_with_basic_properties (msg_, "\5READY", 6);

    return 0;
}

int zmq::plain_server_t::produce_error (msg_t *msg_) const
{
    zmq_assert (status_code.length () == 3);
    const int rc = msg_->init_size (6 + 1 + status_code.length ());
    zmq_assert (rc == 0);
    char *msg_data = static_cast <char *> (msg_->data ());
    memcpy (msg_data, "\5ERROR", 6);
    msg_data [6] = (char) status_code.length ();
    memcpy (msg_data + 7, status_code.c_str (), status_code.length ());
    return 0;
}

void zmq::plain_server_t::send_zap_request (const std::string &username,
                                            const std::string &password)
{
    const uint8_t *credentials[] = {
      reinterpret_cast<const uint8_t *> (username.c_str ()),
      reinterpret_cast<const uint8_t *> (password.c_str ())};
    size_t credentials_sizes[] = {username.size (), password.size ()};
    zap_client_t::send_zap_request ("PLAIN", 5, credentials, credentials_sizes,
                                    2);
}
