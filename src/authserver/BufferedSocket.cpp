/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \file
  \ingroup authserver
  */

#include "BufferedSocket.h"

#include <ace/OS_NS_string.h>
#include <ace/INET_Addr.h>
#include <ace/SString.h>

#ifndef MSG_NOSIGNAL
constexpr auto MSG_NOSIGNAL{ 0 };
#endif

BufferedSocket::BufferedSocket(void): input_buffer_(4096), remote_address_("<unknown>"){}

/*virtual*/ BufferedSocket::~BufferedSocket(void)
{
}

/*virtual*/ int BufferedSocket::open(void* arg)
{
    if (Base::open(arg) == -1)
        return -1;

    ACE_INET_Addr addr;

    if (peer().get_remote_addr(addr) == -1)
        return -1;

    char address[1024];

    addr.get_host_addr(address, 1024);

    this->remote_address_ = address;

    this->OnAccept();

    return 0;
}

const std::string& BufferedSocket::get_remote_address(void) const
{
    return this->remote_address_;
}

size_t BufferedSocket::recv_len(void) const
{
    return this->input_buffer_.length();
}

bool BufferedSocket::recv_soft(char *buf, const size_t len)
{
    if (this->input_buffer_.length() < len)
        return false;

    ACE_OS::memcpy(buf, this->input_buffer_.rd_ptr(), len);

    return true;
}

bool BufferedSocket::recv(char *buf, const size_t len)
{
    const bool ret = this->recv_soft(buf, len);

    if (ret)
        this->recv_skip(len);

    return ret;
}

void BufferedSocket::recv_skip(const size_t len)
{
    this->input_buffer_.rd_ptr(len);
}

ssize_t BufferedSocket::noblk_send(ACE_Message_Block const& message_block)
{
    const size_t len = message_block.length();

    if (len == 0)
        return -1;

    // Try to send the message directly.
    const ssize_t n = this->peer().send(message_block.rd_ptr(), len, MSG_NOSIGNAL);

    if (n < 0)
    {
        if (errno == EWOULDBLOCK)
            return 0;// Blocking signal
        else
            return -1; // Error
    }
    else if (n == 0)
    {
        return -1; // Can this happen?
    }

    // Return bytes transmitted
    return n;
}

bool BufferedSocket::send(const char *buf, const size_t len)
{
    if (buf == nullptr || len == 0)
        return true;

    ACE_Data_Block db(
            len,
            ACE_Message_Block::MB_DATA,
            (const char*)buf,
            0,
            0,
            ACE_Message_Block::DONT_DELETE,
            0);

    ACE_Message_Block message_block(
            &db,
            ACE_Message_Block::DONT_DELETE,
            0);

    message_block.wr_ptr(len);

    if (this->msg_queue()->is_empty())
    {
        // Try to send it directly.
        const ssize_t n = this->noblk_send(message_block);

        if (n < 0)
            return false;
        else if (n == len)
            return true;

        // Adjust how much bytes we sent
        message_block.rd_ptr((size_t)n);

        // Fall down
    }

    // Enqueue the message, note: clone is needed cause we cant enqueue stuff on the stack
    ACE_Message_Block *mb = message_block.clone();

    if (this->msg_queue()->enqueue_tail(mb, (ACE_Time_Value *) &ACE_Time_Value::zero) == -1)
    {
        mb->release();
        return false;
    }

    // Tell reactor to call handle_output() when we can send more data
    return this->reactor()->schedule_wakeup(this, ACE_Event_Handler::WRITE_MASK) != -1;
}

/*virtual*/ int BufferedSocket::handle_output(ACE_HANDLE /*= ACE_INVALID_HANDLE*/)
{
    ACE_Message_Block *mb = 0;

    if (this->msg_queue()->is_empty())
    {
        // If no more data to send, then cancel notification
        this->reactor()->cancel_wakeup(this, ACE_Event_Handler::WRITE_MASK);
        return 0;
    }

    if (this->msg_queue()->dequeue_head(mb, (ACE_Time_Value *) &ACE_Time_Value::zero) == -1)
        return -1;

    const ssize_t n = this->noblk_send(*mb);

    if (n < 0)
    {
        mb->release();
        return -1;
    }
    else if (n == mb->length())
    {
        mb->release();
        return 1;
    }
    else
    {
        mb->rd_ptr(n);

        if (this->msg_queue()->enqueue_head(mb, (ACE_Time_Value *) &ACE_Time_Value::zero) == -1)
        {
            mb->release();
            return -1;
        }

        return 0;
    }

    ACE_NOTREACHED(return -1);
}

/*virtual*/ int BufferedSocket::handle_input(ACE_HANDLE /*= ACE_INVALID_HANDLE*/)
{
    const ssize_t space = this->input_buffer_.space();

    const ssize_t n = this->peer().recv(this->input_buffer_.wr_ptr(), space);

    if (n < 0)
    {
        return errno == EWOULDBLOCK ? 0 : -1; // Blocking signal or error
    }
    else if (n == 0)
    {
        return -1; // EOF
    }

    this->input_buffer_.wr_ptr((size_t)n);

    this->OnRead();

    // Move data in the buffer to the beginning of the buffer
    this->input_buffer_.crunch();

    // Return 1 in case there might be more data to read from OS
    return n == space ? 1 : 0;
}

/*virtual*/ int BufferedSocket::handle_close(ACE_HANDLE /*h*/, ACE_Reactor_Mask /*m*/)
{
    this->OnClose();

    Base::handle_close();

    return 0;
}

void BufferedSocket::close_connection(void)
{
    this->peer().close_reader();
    this->peer().close_writer();

    reactor()->remove_handler(this, ACE_Event_Handler::DONT_CALL | ACE_Event_Handler::ALL_EVENTS_MASK);
}
