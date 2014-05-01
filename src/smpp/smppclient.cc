// Copyright (C) 2011-2014 OnlineCity
// Licensed under the MIT license, which can be read at: http://www.opensource.org/licenses/mit-license.php
// @author hd@onlinecity.dk & td@onlinecity.dk

#include "smpp/smppclient.h"
#include <algorithm>
#include <list>
#include <string>
#include <vector>
#include <utility>
#include <cassert>
#include <functional>

namespace smpp {
using std::string;
using std::vector;
using std::list;
using std::pair;
using std::shared_ptr;
using asio::system_error;
using asio::error_code;
using boost::numeric_cast;
using asio::ip::tcp;
using asio::async_write;
using asio::buffer;

SmppClient::SmppClient(shared_ptr<tcp::socket> socket) :
  system_type_("WWW"),
  interface_version_(0x34),
  addr_ton_(0),
  addr_npi_(0),
  addr_range_(""),
  null_terminate_octet_strings_(true),
  csms_method_(SmppClient::CSMS_16BIT_TAGS),
  msg_ref_callback_(&SmppClient::DefaultMessageRef),
  state_(OPEN),
  socket_(socket),
  seq_no_(0),
  pdu_queue_(),
  socket_write_timeout_(5000),
  socket_read_timeout_(30000),
  verbose_(false) {
  }

SmppClient::~SmppClient() {
  try {
    if (state_ != OPEN) {
      Unbind();
    }
  } catch (std::exception &e) {
  }
}

void SmppClient::BindTransmitter(const string &login, const string &pass) {
  Bind(smpp::BIND_TRANSMITTER, login, pass);
}

void SmppClient::BindReceiver(const string &login, const string &pass) {
  Bind(smpp::BIND_RECEIVER, login, pass);
}

void SmppClient::Bind(uint32_t mode, const string &login, const string &password) {
  CheckConnection();
  CheckState(OPEN);

  PDU pdu = SetupBindPdu(mode, login, password);
  SendCommand(pdu);

  switch (mode) {
    case smpp::BIND_RECEIVER:
      state_ = BOUND_RX;
      break;

    case smpp::BIND_TRANSMITTER:
      state_ = BOUND_TX;
      break;
  }
}

PDU SmppClient::SetupBindPdu(uint32_t mode, const string &login, const string &password) {
  PDU pdu(mode, 0, NextSequenceNumber());
  pdu << login;
  pdu << password;
  pdu << system_type_;
  pdu << interface_version_;
  pdu << addr_ton_;
  pdu << addr_npi_;
  pdu << addr_range_;
  return pdu;
}

void SmppClient::Unbind() {
  CheckConnection();
  PDU pdu(smpp::UNBIND, 0, NextSequenceNumber());
  PDU resp = SendCommand(pdu);
  uint32_t pduStatus = resp.command_status();

  if (pduStatus != smpp::ESME_ROK) {
    throw smpp::SmppException(smpp::GetEsmeStatus(pduStatus));
  }

  state_ = OPEN;
}

pair<string, int> SmppClient::SendSms(
    const SmppAddress &sender,
    const SmppAddress &receiver,
    const string &short_message) {
  struct SmppParams params;
  return SendSms(sender, receiver, short_message, params, list<TLV>());
}

pair<string, int> SmppClient::SendSms(
    const SmppAddress &sender,
    const SmppAddress &receiver,
    const string &short_message,
    const struct SmppParams &params) {
  return SendSms(sender, receiver, short_message, params, list<TLV>());
}

pair<string, int> SmppClient::SendSms(
    const SmppAddress &sender,
    const SmppAddress &receiver,
    const string &short_message,
    const struct SmppParams &params,
    list<TLV> tags)
{
  int message_len = short_message.length();
  int single_sms_octet_limit = 254;  // Default SMPP standard
  int csms_split = -1;  // where to split

  switch (params.data_coding) {
    case smpp::DATA_CODING_UCS2:
      single_sms_octet_limit = 140;
      csms_split = 132;
      break;

    case smpp::DATA_CODING_DEFAULT:
      single_sms_octet_limit = 160;
      csms_split = 152;
      break;
  }

  // submit_sm if the short message could fit into one pdu.
  if (message_len <= single_sms_octet_limit || csms_method_ == CSMS_PAYLOAD) {
    string smsc_id = SubmitSm(sender, receiver, short_message, params, tags);
    return std::make_pair(smsc_id, 1);
  }

  // CSMS -> split message
  vector<string> parts = Split(short_message, csms_split);
  vector<string>::iterator itr = parts.begin();

  if (csms_method_ == CSMS_8BIT_UDH) {
    // encode an udh with an 8bit csms reference
    uint8_t segment = 0;
    uint8_t segments = numeric_cast<uint8_t>(parts.size());
    string sms_id;
    uint8_t csms_ref = static_cast<uint8_t>(msg_ref_callback_() & 0xff);

    for (; itr < parts.end(); itr++) {
      // encode udh
      int part_size = (*itr).size();
      int size = 6 + part_size;
      std::unique_ptr<uint8_t[]> udh(new uint8_t[size]);
      udh[0] = 0x05;  // length of udh excluding first byte
      udh[1] = 0x00;  //
      udh[2] = 0x03;  // length of the header
      udh[3] = csms_ref;
      udh[4] = segments;
      udh[5] = ++segment;
      // concatenate with message part
      copy((*itr).begin(), (*itr).end(), &udh[6]);
      string message(reinterpret_cast<char*>(udh.get()), size);
      sms_id = SubmitSm(sender, receiver, message, params, tags);
    }
    return std::make_pair(sms_id, segments);
  } else {  // csmsMethod == CSMS_16BIT_TAGS)
    tags.push_back(TLV(smpp::tags::SAR_MSG_REF_NUM, static_cast<uint16_t>(msg_ref_callback_())));
    tags.push_back(TLV(smpp::tags::SAR_TOTAL_SEGMENTS, boost::numeric_cast<uint8_t>(parts.size())));
    int segment = 0;
    string sms_id;

    for (; itr < parts.end(); ++itr) {
      tags.push_back(TLV(smpp::tags::SAR_SEGMENT_SEQNUM, ++segment));
      sms_id = SubmitSm(sender, receiver, (*itr), params, tags);
      // pop SAR_SEGMENT_SEQNUM tag
      tags.pop_back();
    }

    // pop SAR_TOTAL_SEGMENTS tag
    tags.pop_back();
    // pop SAR_MSG_REF_NUM tag
    tags.pop_back();
    return std::make_pair(sms_id, segment);
  }
}

SMS SmppClient::ReadSms() {
  // see if we're bound correct.
  CheckState(BOUND_RX);

  // if  there are any messages in the queue pop the first usable one off and return it
  if (!pdu_queue_.empty()) {
    return ParseSms();
  }

  // fill queue until we get a DELIVER_SM command
  try {
    bool b = false;

    while (!b) {
      PDU pdu = ReadPdu(true);

      if (pdu.command_id() == ENQUIRE_LINK) {
        PDU resp = PDU(ENQUIRE_LINK_RESP, 0, pdu.sequence_no());
        SendPdu(resp);
        continue;
      }

      if (pdu.null()) {
        break;
      }

      if (!pdu.null()) {
        pdu_queue_.push_back(pdu);    // save pdu for reading later
      }

      b = pdu.command_id() == DELIVER_SM;
    }
  } catch (std::exception &e) {
    throw smpp::TransportException(e.what());
  }

  return ParseSms();
}

QuerySmResult SmppClient::QuerySm(std::string messageid, SmppAddress source) {
  PDU pdu = PDU(QUERY_SM, 0, NextSequenceNumber());
  pdu << messageid;
  pdu << source.ton;
  pdu << source.npi;
  pdu << source.value;
  PDU reply = SendCommand(pdu);
  string msgid;
  string final_date;
  uint8_t message_state;
  uint8_t error_code;
  reply >> msgid;
  reply >> final_date;
  reply >> message_state;
  reply >> error_code;
  std::chrono::time_point<std::chrono::system_clock> tp;

  if (final_date.length() > 1) {
    smpp::timeformat::ChronoDatePair p = smpp::timeformat::ParseSmppTimestamp(final_date);
    tp = p.first;
  }

  return QuerySmResult(msgid, tp, message_state, error_code);
}

void SmppClient::EnquireLink() {
  PDU pdu = PDU(ENQUIRE_LINK, 0, NextSequenceNumber());
  SendCommand(pdu);
}

SMS SmppClient::ParseSms() {
  if (pdu_queue_.empty()) {
    return SMS();
  }

  list<PDU>::iterator it = pdu_queue_.begin();

  while (it != pdu_queue_.end()) {
    if ((*it).command_id() == DELIVER_SM) {
      SMS sms(*it);
      // send response to smsc
      PDU resp = PDU(DELIVER_SM_RESP, 0x0, (*it).sequence_no());
      resp << 0x0;
      SendPdu(resp);
      // remove sms from queue
      pdu_queue_.erase(it);
      return sms;
    }

    if ((*it).command_id() == ALERT_NOTIFICATION) {
      it = pdu_queue_.erase(it);
      continue;
    }

    if ((*it).command_id() == DATA_SM) {
      PDU resp = PDU(DATA_SM_RESP, 0x0, (*it).sequence_no());
      resp << 0x0;
      SendPdu(resp);
      it = pdu_queue_.erase(it);
      continue;
    }
    ++it;
  }
  return SMS();
}

vector<string> SmppClient::Split(const string &short_message, const int split) {
  vector<string> parts;
  int len = short_message.length();
  int pos = 0;
  int n = split;

  while (pos < len) {
    if (static_cast<int>(short_message[pos + n - 1]) == 0x1b) {  // do not split at escape char
      n--;
    }

    parts.push_back(short_message.substr(pos, n));
    pos += n;
    n = split;

    if (pos + n > len) {
      n = len - pos;
    }
  }
  return parts;
}

string SmppClient::SubmitSm(const SmppAddress &sender, const SmppAddress &receiver, const string &short_message, const struct SmppParams &params, list<TLV> tags) {
  CheckState(BOUND_TX);
  PDU pdu(smpp::SUBMIT_SM, 0, NextSequenceNumber());
  pdu << params.service_type;
  pdu << sender;
  pdu << receiver;
  if (csms_method_ == CSMS_8BIT_UDH) {
    pdu << (params.esm_class | ESM_UHDI);  // Set UHDI bit
  } else {
    pdu << params.esm_class;
  }
  pdu << params.protocol_id;
  pdu << params.priority_flag;
  pdu << params.schedule_delivery_time;
  pdu << params.validity_period;
  pdu << params.registered_delivery;
  pdu << params.replace_if_present_flag;
  pdu << params.data_coding;
  pdu << params.sm_default_msg_id;

  if (csms_method_ == CSMS_PAYLOAD) {
    pdu << 0;  // sm_length = 0
    pdu << TLV(smpp::tags::MESSAGE_PAYLOAD, short_message);
  } else {
    pdu.set_null_terminate_octet_strings(null_terminate_octet_strings_);
    pdu << boost::numeric_cast<uint8_t>(short_message.length()) + (null_terminate_octet_strings_ ? 1 : 0);
    pdu << short_message;
    pdu.set_null_terminate_octet_strings(true);
  }

  // add  optional tags.
  for (list<TLV>::iterator itr = tags.begin(); itr != tags.end(); itr++) {
    pdu << *itr;
  }

  PDU resp = SendCommand(pdu);
  string messageid;
  resp >> messageid;
  return messageid;
}

uint32_t SmppClient::NextSequenceNumber() {
  if (++seq_no_ > 0x7FFFFFFF) {
    throw SmppException("Ran out of sequence numbers");
  }
  return seq_no_;
}

void SmppClient::SendPdu(PDU &pdu) {
  CheckConnection();
  bool io_result = false;
  bool timer_result = false;

  if (verbose_) {
    LOG(INFO) << pdu;
  }

  smpp::ChronoDeadlineTimer timer(socket_->get_io_service());
  timer.expires_from_now(std::chrono::milliseconds(socket_write_timeout_));
  timer.async_wait(std::bind(&SmppClient::HandleTimeout, this, &timer_result, std::placeholders::_1));

  async_write(*socket_,
      buffer(static_cast<const void*>(pdu.getOctets().c_str()),
      pdu.Size()),
      std::bind(&SmppClient::WriteHandler, this, &io_result, std::placeholders::_1));

  SocketExecute();

  if (io_result) {
    timer.cancel();
  } else if (timer_result) {
    socket_->cancel();
  }

  SocketExecute();
}

PDU SmppClient::SendCommand(PDU &pdu) {
  SendPdu(pdu);
  PDU resp = ReadPduResponse(pdu.sequence_no(), pdu.command_id());

  switch (resp.command_status()) {
    case smpp::ESME_RINVPASWD:
      throw smpp::InvalidPasswordException(smpp::GetEsmeStatus(resp.command_status()));
      break;
    case smpp::ESME_RINVSYSID:
      throw smpp::InvalidSystemIdException(smpp::GetEsmeStatus(resp.command_status()));
      break;
    case smpp::ESME_RINVSRCADR:
      throw smpp::InvalidSourceAddressException(smpp::GetEsmeStatus(resp.command_status()));
      break;
    case smpp::ESME_RINVDSTADR:
      throw smpp::InvalidDestinationAddressException(smpp::GetEsmeStatus(resp.command_status()));
      break;
  }

  if (resp.command_status() != smpp::ESME_ROK) {
    throw smpp::SmppException(smpp::GetEsmeStatus(resp.command_status()));
  }

  return resp;
}

PDU SmppClient::ReadPdu(const bool &isBlocking) {
  // return NULL pdu if there is nothing on the wire for us.
  if (!isBlocking && !SocketPeek()) {
    return PDU();
  }

  ReadPduBlocking();

  // There are no pdus to be read return a null pdu.
  if (pdu_queue_.empty()) {
    return PDU();
  }

  // Return last the pdu inserted into the queue.
  PDU pdu = pdu_queue_.back();
  pdu_queue_.pop_back();

  if (verbose_) {
    LOG(INFO) << pdu;
  }

  return pdu;
}

bool SmppClient::SocketPeek() {
  // prepare our read
  PduLengthHeader pdu_header;
  async_read(*socket_, asio::buffer(pdu_header),
      std::bind(&smpp::SmppClient::ReadPduHeaderHandler, this, std::placeholders::_1,std::placeholders::_2, &pdu_header));
  size_t handlers_called = socket_->get_io_service().poll_one();
  socket_->get_io_service().reset();
  socket_->cancel();
  SocketExecute();
  return handlers_called != 0;
}

void SmppClient::ReadPduBlocking() {
  bool io_result = false;
  bool timer_result = false;
  PduLengthHeader pdu_header;
  async_read(*socket_,
      asio::buffer(pdu_header),
      std::bind(&SmppClient::ReadPduHeaderHandlerBlocking,
        this,
        &io_result,
        std::placeholders::_1,
        std::placeholders::_2,
        &pdu_header));

  smpp::ChronoDeadlineTimer timer(socket_->get_io_service());
  timer.expires_from_now(std::chrono::milliseconds(socket_read_timeout_));
  timer.async_wait(std::bind(&SmppClient::HandleTimeout, this, &timer_result, std::placeholders::_1));
  SocketExecute();

  if (io_result) {
    timer.cancel();
  } else if (timer_result) {
    socket_->cancel();
  }

  SocketExecute();
}

void SmppClient::HandleTimeout(bool *had_error, const error_code &error) {
  *had_error = true;
}

void SmppClient::WriteHandler(bool *had_error, const error_code &error) {
  *had_error = true;
  if (error) {
    throw TransportException(system_error(error).what());
  }
}

void SmppClient::SocketExecute() {
  socket_->get_io_service().run_one();
  socket_->get_io_service().reset();
}

void SmppClient::ReadPduHeaderHandler(const error_code &error, size_t len, const PduLengthHeader *pduLength) {
  if (error) {
    if (error == asio::error::operation_aborted) {
      // Not treated as an error
      return;
    }

    throw TransportException(system_error(error).what());
  }

  assert(len == 4);

  uint32_t i = PDU::GetPduLength(*pduLength);
  PduData pdu_buffer;
  pdu_buffer.resize(i);
  // start reading after the size mark of the pdu
  async_read(*socket_,
      buffer(&*pdu_buffer.begin(), i - 4),
      std::bind(&smpp::SmppClient::ReadPduBodyHandler, this, std::placeholders::_1, std::placeholders::_2, pduLength, &pdu_buffer));
  SocketExecute();
}

void SmppClient::ReadPduHeaderHandlerBlocking(bool *had_error, const error_code &error, size_t read,
    const PduLengthHeader *pduLength) {
  if (error) {
    if (error == asio::error::operation_aborted) {
      // Not treated as an error
      return;
    }

    throw TransportException(system_error(error).what());
  }

  *had_error = true;
  uint32_t i = PDU::GetPduLength(*pduLength);
  PduData pdu_buffer;
  pdu_buffer.resize(i - 4);
  // start reading after the size mark of the pdu
  async_read(*socket_, buffer(static_cast<char*>(&*pdu_buffer.begin()), i - 4),
      std::bind(&smpp::SmppClient::ReadPduBodyHandler, this, std::placeholders::_1, std::placeholders::_2, pduLength, &pdu_buffer));
  SocketExecute();
}

void SmppClient::ReadPduBodyHandler(const error_code &error, size_t len, const PduLengthHeader *pduLength, const PduData *pdu_buffer) {
  if (error) {
    throw TransportException(system_error(error).what());
  }

  pdu_queue_.emplace_back(*pduLength, *pdu_buffer);
}

// blocks until response is read
PDU SmppClient::ReadPduResponse(const uint32_t &sequence, const uint32_t &commandId) {
  uint32_t response = GENERIC_NACK | commandId;
  list<PDU>::iterator it = pdu_queue_.begin();

  while (it != pdu_queue_.end()) {
    PDU pdu = (*it);
    if (pdu.sequence_no() == sequence && pdu.command_id() == response) {
      it = pdu_queue_.erase(it);
      return pdu;
    }

    it++;
  }

  while (true) {
    PDU pdu = ReadPdu(true);
    if (!pdu.null()) {
      if ((pdu.sequence_no() == sequence && (pdu.command_id() == response
              || pdu.command_id() == GENERIC_NACK))
          || (pdu.sequence_no() == 0 && pdu.command_id() == GENERIC_NACK)) {
        return pdu;
      }
    }
  }

  PDU pdu;
  return pdu;
}

void smpp::SmppClient::EnquireLinkRespond() {
  list<PDU>::iterator it = pdu_queue_.begin();

  while (it != pdu_queue_.end()) {
    PDU pdu = (*it);
    if (pdu.command_id() == ENQUIRE_LINK) {
      PDU resp = PDU(ENQUIRE_LINK_RESP, 0, pdu.sequence_no());
      SendCommand(resp);
    }
    it++;
  }

  PDU pdu = ReadPdu(false);

  if (!pdu.null() && pdu.command_id() == ENQUIRE_LINK) {
    PDU resp = PDU(ENQUIRE_LINK_RESP, 0, pdu.sequence_no());
    SendPdu(resp);
  }
}

void SmppClient::CheckConnection() {
  if (!socket_->is_open()) {
    throw smpp::TransportException("Socket is closed");
  }
}

void SmppClient::CheckState(int state) {
  if (this->state_ != state) {
    throw smpp::SmppException("Client in wrong state");
  }
}

uint16_t SmppClient::DefaultMessageRef() {
  static int ref = 0;
  return (ref++ % 0xffff);
}
}  // namespace smpp
