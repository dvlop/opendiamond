#
#  The OpenDiamond Platform for Interactive Search
#  Version 6
#
#  Copyright (c) 2011 Carnegie Mellon University
#  All rights reserved.
#
#  This software is distributed under the terms of the Eclipse Public
#  License, Version 1.0 which can be found in the file named LICENSE.
#  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
#  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
#

'''Low-level RPC protocol implementation.'''

from __future__ import with_statement
import logging
import socket
import struct
import threading
from xdrlib import Packer, Unpacker, Error as XDRError

_log = logging.getLogger(__name__)

RPC_PENDING = -1

class ConnectionFailure(Exception):
    '''RPC connection died.'''
class RPCError(Exception):
    '''Base class for RPC error codes.'''
class RPCEncodingError(RPCError):
    '''Bad XDR structure.'''
    code = -2
class RPCProcedureUnavailable(RPCError):
    '''Remote procedure not available.'''
    code = -3


class XDREncodable(object):
    '''Base class for an XDR-encodable data structure.'''

    def encode(self, xdr):
        '''Serialize the object into an XDR stream.'''
        raise NotImplementedError()

    @staticmethod
    def encode_int(xdr, val):
        '''Due to Python #9696, packer.pack_int() fails for negative values
        on Python < 2.7.2.  Work around this.'''
        xdr.pack_fstring(4, struct.pack('>i', val))

    @staticmethod
    def encode_array(xdr, items):
        '''Packer.pack_array() is inconvenient for recursive descent.  This
        method is more convenient.'''
        xdr.pack_uint(len(items))
        for item in items:
            item.encode(xdr)


class _RPCHeader(XDREncodable):
    '''An RPC message header.'''

    def __init__(self, xdr=None, sequence=None, status=None, cmd=None,
                        datalen=None):
        XDREncodable.__init__(self)
        if xdr is not None:
            self.sequence = xdr.unpack_uint()
            self.status = xdr.unpack_int()
            self.cmd = xdr.unpack_int()
            self.datalen = xdr.unpack_uint()
        else:
            self.sequence = sequence
            self.status = status
            self.cmd = cmd
            self.datalen = datalen

    def encode(self, xdr):
        xdr.pack_uint(self.sequence)
        self.encode_int(xdr, self.status)
        self.encode_int(xdr, self.cmd)
        xdr.pack_uint(self.datalen)


class _RPCRequest(object):
    '''The header and data from an RPC request.'''

    def __init__(self, hdr, data):
        self.hdr = hdr
        self.data = data

    def make_reply_header(self, status, data):
        '''Return the header for an RPC reply.'''
        return _RPCHeader(sequence=self.hdr.sequence, status=status,
                            cmd=self.hdr.cmd, datalen=len(data))


class RPCConnection(object):
    '''An RPC connection.'''

    def __init__(self, sock):
        self._sock = sock
        self._lock = threading.Lock()

    def _receive(self):
        def read_bytes(count):
            '''self._lock must be held.'''
            bufs = []
            try:
                while count > 0:
                    new = self._sock.recv(count)
                    if len(new) == 0:
                        self._sock.close()
                        raise ConnectionFailure('Short read')
                    count -= len(new)
                    bufs.append(new)
            except socket.error, e:
                self._sock.close()
                raise ConnectionFailure(str(e))
            return ''.join(bufs)

        while True:
            with self._lock:
                hdr = _RPCHeader(xdr=Unpacker(read_bytes(16)))
                data = read_bytes(hdr.datalen)
            # We only handle request traffic; ignore reply messages
            if hdr.status == RPC_PENDING:
                return _RPCRequest(hdr, data)

    def _reply(self, request, status=0, body=''):
        assert status == 0 or len(body) == 0
        xdr = Packer()
        request.make_reply_header(status, body).encode(xdr)
        with self._lock:
            try:
                self._sock.sendall(xdr.get_buffer() + body)
            except socket.error, e:
                self._sock.close()
                raise ConnectionFailure(str(e))

    def dispatch(self, handlers):
        '''Receive an RPC request, call a handler in handlers to process it,
        and transmit the reply.'''
        req = self._receive()
        try:
            # Look up handler and decode request
            handler_name = 'Command %d' % req.hdr.cmd
            try:
                handler = handlers.get_handler(req.hdr.cmd)
                handler_name = (handler.im_class.__name__ + '.' +
                                handler.__name__)
                if handler.rpc_request_class is not None:
                    unpacker = Unpacker(req.data)
                    req_obj = handler.rpc_request_class(xdr=unpacker)
                    unpacker.done()
            except KeyError:
                raise RPCProcedureUnavailable()
            except (XDRError, ValueError, EOFError, struct.error):
                raise RPCEncodingError()

            # Call handler
            if handler.rpc_request_class is not None:
                ret_obj = handler(req_obj)
            else:
                ret_obj = handler()

            # Encode reply
            if ret_obj is None:
                assert handler.rpc_reply_class is None
                ret = ''
            else:
                assert isinstance(ret_obj, handler.rpc_reply_class)
                xdr = Packer()
                ret_obj.encode(xdr)
                ret = xdr.get_buffer()

            # Send reply
            self._reply(req, body=ret)
            if handlers.log_rpcs:
                _log.debug('%s => success', handler_name)
        except RPCError, e:
            self._reply(req, status=e.code)
            if handlers.log_rpcs:
                _log.debug('%s => %s', handler_name, e.__class__.__name__)


# _RPCMeta accesses a protected member of the classes it controls
# pylint: disable=W0212
class _RPCMeta(type):
    '''Metaclass for RPCHandlers that collects the methods tagged with
    @RPCHandlers.handler into a dictionary.'''

    def __new__(mcs, name, bases, dct):
        obj = type.__new__(mcs, name, bases, dct)
        obj._cmds = dict()
        for name in dir(obj):
            f = getattr(obj, name)
            if hasattr(f, 'rpc_procedure'):
                # f is an unbound method
                obj._cmds[f.rpc_procedure] = f
        return obj
# pylint: enable=W0212


class RPCHandlers(object):
    '''Base class of RPC handler objects.'''

    __metaclass__ = _RPCMeta
    log_rpcs = False

    @staticmethod
    def handler(cmd, request_class=None, reply_class=None):
        '''Decorator declaring the function to be an RPC handler with the
        given command number and request class.'''
        def decorator(func):
            func.rpc_procedure = cmd
            func.rpc_request_class = request_class
            func.rpc_reply_class = reply_class
            return func
        return decorator

    # self._cmds is created by _RPCMeta
    # pylint: disable=E1101
    def get_handler(self, procedure):
        '''Returns the handler function for the specified procedure number
        or raises KeyError.'''
        # self._cmds contains unbound methods; return a bound one
        return self._cmds[procedure].__get__(self, self.__class__)
    # pylint: enable=E1101