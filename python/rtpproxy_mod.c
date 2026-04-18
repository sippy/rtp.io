#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#define PY_SSIZE_T_CLEAN
#include <sys/types.h>
#include <sys/socket.h>
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
/* Get rid of annoying warning in cpython/pytime.h */
struct timespec;
extern char *strdup(const char *str);
#endif
#include <Python.h>
#include <structmember.h>

#include <librtpproxy.h>
#include <librtpproxy/packet_ext.h>
#include <librtpproxy/packetport.h>

#include "rtpproxy_mod.h"

#define CACHELINE_SIZE 64

#define MODULE_PREFIX rtp
#define MODULE_BASENAME io

#define CONCATENATE_DETAIL(x, y) x##y
#define CONCATENATE(x, y) CONCATENATE_DETAIL(x, y)

#if defined(DEBUG_MOD)
#define MODULE_BASENAME CONCATENATE(MODULE_BASENAME, _debug)
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define MODULE_NAME_STR TOSTRING(MODULE_PREFIX) "." TOSTRING(MODULE_BASENAME)
#define PY_INIT_FUNC CONCATENATE(PyInit_, MODULE_BASENAME)
#define CLS_NAME_STR "rtpproxy"

struct RTPPSocket {
    struct {
        int our; // Raw fd
        int their;
    } fds;
    struct {
      PyObject *rtpp_sock; // Strong reference to Python socket object
      PyObject *spec_str;  // Python string object for rtpp_spec
    } py;
    char rtpp_spec[16];
};

typedef struct {
    PyObject_HEAD
    struct rtpp_cfg *rtpp_ctx;
    struct RTPPSocket cmd;
    struct RTPPSocket notify;
    struct RTPPInitializeParams rp;
    char ttl_val[16];
    char setup_ttl_val[16];
    char port_min[16];
    char port_max[16];
    const char *_modules[MAX_MODULES + 1];
    const char *_extra_args[MAX_EXTRA_ARGS + 1];
} PyRTPProxyObject;

typedef struct {
    PyObject_HEAD
    struct rtpp_packetport *packetport;
    struct rtp_packet_ext **push_batch;
    struct rtp_packet_ext **pop_batch;
    unsigned int capacity;
} PyRTPPPacketPortObject;

typedef struct {
    PyObject_HEAD
    struct rtp_packet_ext *pktxp;
} PyRTPPPacketExtObject;

static const char *default_modules[] = {
    "acct_csv", "catch_dtmf", "dtls_gw", "ice_lite", NULL
};

static const struct RTPPInitializeParams RTPPInitializeParams = {
    .ttl = -1,
    .setup_ttl = -1,
    .socket = NULL,
    .debug_level = "info",
    .notify_socket = "tcp:127.0.0.1:9642",
    .rec_spool_dir = "/tmp",
    .rec_final_dir = ".",
    .modules = default_modules,
};

static PyTypeObject PyRTPPPacketPortType;
static PyTypeObject PyRTPPPacketExtType;

static struct rtp_packet_ext **
packet_batch_alloc(unsigned int capacity)
{
    struct rtp_packet_ext **batch;

    if (posix_memalign((void **)&batch, CACHELINE_SIZE,
      sizeof(batch[0]) * (size_t)capacity) != 0) {
        return NULL;
    }
    return batch;
}

static struct rtp_packet_ext *
PyRTPPPacketExt_from_bytes(const void *data, Py_ssize_t dlen, unsigned int port)
{
    struct rtp_packet_ext *pktxp;

    if (dlen <= 0) {
        PyErr_SetString(PyExc_ValueError, "data must not be empty");
        return (NULL);
    }
    if (dlen > INT_MAX) {
        PyErr_SetString(PyExc_ValueError, "data is too large");
        return (NULL);
    }
    pktxp = rtp_packet_ext_ctor((int)dlen, port, data, NULL, NULL);
    if (pktxp == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "rtp_packet_ext_ctor() failed");
        return (NULL);
    }
    return (pktxp);
}

static struct rtp_packet_ext *
PyRTPPPacketExt_from_ctor_arg(PyObject *arg, unsigned int port)
{
    struct rtp_packet_ext *pktxp;
    char *data = NULL;
    Py_ssize_t dlen;
    long lval;

    if (PyBytes_Check(arg)) {
        if (PyBytes_AsStringAndSize(arg, &data, &dlen) != 0) {
            return (NULL);
        }
        return (PyRTPPPacketExt_from_bytes(data, dlen, port));
    } else {
        lval = PyLong_AsLong(arg);
        if (lval == -1 && PyErr_Occurred() != NULL) {
            PyErr_SetString(PyExc_TypeError, "data must be bytes or size");
            return (NULL);
        }
        if (lval <= 0) {
            PyErr_SetString(PyExc_ValueError, "size must be greater than 0");
            return (NULL);
        }
        dlen = lval;
    }
    pktxp = PyRTPPPacketExt_from_bytes(NULL, dlen, port);
    if (pktxp == NULL) {
        return (NULL);
    }
    return (pktxp);
}

static PyObject *
PyRTPPPacketPort_FromRTPP(struct rtpp_packetport *packetport,
  unsigned int capacity)
{
    PyRTPPPacketPortObject *self;

    self = PyObject_New(PyRTPPPacketPortObject, &PyRTPPPacketPortType);
    if (self == NULL) {
        rtpp_packetport_dtor(packetport);
        return NULL;
    }
    self->packetport = packetport;
    self->capacity = capacity;
    self->push_batch = packet_batch_alloc(capacity);
    if (self->push_batch == NULL) {
        rtpp_packetport_dtor(packetport);
        PyObject_Del(self);
        PyErr_NoMemory();
        return NULL;
    }
    self->pop_batch = packet_batch_alloc(capacity);
    if (self->pop_batch == NULL) {
        free(self->push_batch);
        self->push_batch = NULL;
        rtpp_packetport_dtor(packetport);
        PyObject_Del(self);
        PyErr_NoMemory();
        return NULL;
    }
    return (PyObject *)self;
}

static PyObject *
PyRTPPPacketExt_FromRTPP(struct rtp_packet_ext *pktxp)
{
    PyRTPPPacketExtObject *self;

    self = PyObject_New(PyRTPPPacketExtObject, &PyRTPPPacketExtType);
    if (self == NULL) {
        rtp_packet_ext_dtor(pktxp);
        return NULL;
    }
    self->pktxp = pktxp;
    return (PyObject *)self;
}

static PyObject *
PyRTPPPacketPort_address_ref(PyRTPPPacketPortObject *self, PyObject *args)
{
    char buf[64];

    (void)args;
    snprintf(buf, sizeof(buf), "RTQ:%p", (void *)self->packetport);
    return PyUnicode_FromString(buf);
}

static PyObject *
PyRTPPPacketPort_next_in_port(PyRTPPPacketPortObject *self, PyObject *args)
{
    unsigned int port;

    (void)args;
    port = rtpp_packetport_next_in_port(self->packetport);
    return PyLong_FromUnsignedLong(port);
}

static PyObject *
PyRTPPPacketPort_push(PyRTPPPacketPortObject *self, PyObject *args)
{
    struct rtp_packet_ext *pktxp;
    PyObject *arg0, *dataobj;
    char *data;
    Py_ssize_t dlen;
    unsigned int port;

    if (PyTuple_Size(args) == 1) {
        if (!PyArg_ParseTuple(args, "O", &arg0)) {
            return NULL;
        }
        if (!PyObject_TypeCheck(arg0, &PyRTPPPacketExtType)) {
            PyErr_SetString(PyExc_TypeError,
              "push(packet) expects an rtp.io.packet");
            return NULL;
        }
        pktxp = ((PyRTPPPacketExtObject *)arg0)->pktxp;
        rtpp_packetport_push(self->packetport, pktxp);
        Py_RETURN_NONE;
    }
    if (!PyArg_ParseTuple(args, "OI", &dataobj, &port)) {
        return NULL;
    }
    if (!PyBytes_Check(dataobj)) {
        PyErr_SetString(PyExc_TypeError, "data must be bytes");
        return NULL;
    }
    if (PyBytes_AsStringAndSize(dataobj, &data, &dlen) != 0) {
        return NULL;
    }
    pktxp = PyRTPPPacketExt_from_bytes(data, dlen, port);
    if (pktxp == NULL) {
        return NULL;
    }
    rtpp_packetport_push(self->packetport, pktxp);
    rtp_packet_ext_dtor(pktxp);
    Py_RETURN_NONE;
}

static PyObject *
PyRTPPPacketPort_try_push(PyRTPPPacketPortObject *self, PyObject *args)
{
    struct rtp_packet_ext *pktxp;
    PyObject *arg0, *dataobj;
    char *data;
    Py_ssize_t dlen;
    unsigned int port;

    if (PyTuple_Size(args) == 1) {
        if (!PyArg_ParseTuple(args, "O", &arg0)) {
            return (NULL);
        }
        if (!PyObject_TypeCheck(arg0, &PyRTPPPacketExtType)) {
            PyErr_SetString(PyExc_TypeError,
              "try_push(packet) expects an rtp.io.packet");
            return (NULL);
        }
        pktxp = ((PyRTPPPacketExtObject *)arg0)->pktxp;
        if (rtpp_packetport_try_push(self->packetport, pktxp) != 0) {
            PyErr_SetString(PyExc_BufferError, "packetport queue is full");
            return (NULL);
        }
        Py_RETURN_NONE;
    }
    if (!PyArg_ParseTuple(args, "OI", &dataobj, &port)) {
        return (NULL);
    }
    if (!PyBytes_Check(dataobj)) {
        PyErr_SetString(PyExc_TypeError, "data must be bytes");
        return (NULL);
    }
    if (PyBytes_AsStringAndSize(dataobj, &data, &dlen) != 0) {
        return (NULL);
    }
    pktxp = PyRTPPPacketExt_from_bytes(data, dlen, port);
    if (pktxp == NULL) {
        return (NULL);
    }
    if (rtpp_packetport_try_push(self->packetport, pktxp) != 0) {
        rtp_packet_ext_dtor(pktxp);
        PyErr_SetString(PyExc_BufferError, "packetport queue is full");
        return (NULL);
    }
    rtp_packet_ext_dtor(pktxp);
    Py_RETURN_NONE;
}

static PyObject *
PyRTPPPacketPort_try_push_many(PyRTPPPacketPortObject *self, PyObject *arg)
{
    PyObject *items;
    Py_ssize_t nitems;
    size_t pushed;
    Py_ssize_t i;

    items = PySequence_Fast(arg, "try_push_many(packets) expects a sequence");
    if (items == NULL) {
        return (NULL);
    }
    nitems = PySequence_Fast_GET_SIZE(items);
    if (nitems < 0 || (unsigned long)nitems > (unsigned long)self->capacity) {
        Py_DECREF(items);
        PyErr_SetString(PyExc_ValueError,
          "packet batch size must be in range [0, capacity]");
        return (NULL);
    }
    if (nitems == 0) {
        Py_DECREF(items);
        PyErr_SetString(PyExc_ValueError, "packet batch must not be empty");
        return (NULL);
    }
    for (i = 0; i < nitems; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(items, i);

        if (!PyObject_TypeCheck(item, &PyRTPPPacketExtType)) {
            Py_DECREF(items);
            PyErr_SetString(PyExc_TypeError,
              "try_push_many(packets) expects rtp.io.packet items");
            return (NULL);
        }
        self->push_batch[i] = ((PyRTPPPacketExtObject *)item)->pktxp;
    }
    Py_BEGIN_ALLOW_THREADS
    pushed = rtpp_packetport_try_push_many(self->packetport, self->push_batch,
      (size_t)nitems);
    Py_END_ALLOW_THREADS
    Py_DECREF(items);
    return (PyLong_FromSize_t(pushed));
}

static PyObject *
PyRTPPPacketPort_try_pop(PyRTPPPacketPortObject *self, PyObject *args)
{
    struct rtp_packet_ext *pktxp;

    (void)args;
    pktxp = rtpp_packetport_try_pop(self->packetport);
    if (pktxp == NULL) {
        Py_RETURN_NONE;
    }
    return PyRTPPPacketExt_FromRTPP(pktxp);
}

static PyObject *
PyRTPPPacketPort_try_pop_many(PyRTPPPacketPortObject *self, PyObject *args)
{
    PyObject *ret;
    size_t nitems;
    Py_ssize_t i, dtor_from = 0;
    unsigned int max_items;

    max_items = self->capacity;
    if (!PyArg_ParseTuple(args, "|I", &max_items)) {
        return (NULL);
    }
    if (max_items == 0 || max_items > self->capacity) {
        PyErr_SetString(PyExc_ValueError, "max_items is out of range");
        return (NULL);
    }
    Py_BEGIN_ALLOW_THREADS
    nitems = rtpp_packetport_try_pop_many(self->packetport, self->pop_batch,
      max_items);
    Py_END_ALLOW_THREADS
    ret = PyTuple_New((Py_ssize_t)nitems);
    if (ret == NULL) {
        goto e0;
    }
    for (i = 0; i < (Py_ssize_t)nitems; i++) {
        PyObject *pktxo;

        assert(self->pop_batch[i] != NULL);
        pktxo = PyRTPPPacketExt_FromRTPP(self->pop_batch[i]);
        if (pktxo == NULL) {
            dtor_from = i;
            goto e1;
        }
        PyTuple_SET_ITEM(ret, i, pktxo);
    }
    return (ret);
e1:
    Py_DECREF(ret);
e0:
    for (i = dtor_from; i < (Py_ssize_t)nitems; i++) {
        rtp_packet_ext_dtor(self->pop_batch[i]);
    }
    return (NULL);
}

static PyObject *
PyRTPPPacketPort_flush(PyRTPPPacketPortObject *self, PyObject *args)
{
    int rval;

    (void)args;
    Py_BEGIN_ALLOW_THREADS
    rval = rtpp_packetport_flush(self->packetport);
    Py_END_ALLOW_THREADS
    if (rval != 0) {
        PyErr_SetString(PyExc_RuntimeError, "rtpp_packetport_flush() failed");
        return (NULL);
    }
    Py_RETURN_NONE;
}

static int
PyRTPPPacketExt_getbuffer(PyObject *obj, Py_buffer *view, int flags)
{
    PyRTPPPacketExtObject *self;

    self = (PyRTPPPacketExtObject *)obj;
    return PyBuffer_FillInfo(view, obj, (void *)self->pktxp->data,
      (Py_ssize_t)self->pktxp->dlen, !rtpp_packet_ext_owns_data(self->pktxp),
      flags);
}

static PyObject *
PyRTPPPacketExt_get_port(PyRTPPPacketExtObject *self, void *closure)
{

    (void)closure;
    return PyLong_FromUnsignedLong(self->pktxp->port);
}

static PyObject *
PyRTPPPacketExt_get__data_ptr(PyRTPPPacketExtObject *self, void *closure)
{

    (void)closure;
    return PyLong_FromVoidPtr((void *)self->pktxp->data);
}

static PyObject *
PyRTPPPacketExt_get_rtime(PyRTPPPacketExtObject *self, void *closure)
{
    double wall, mono;

    (void)closure;
    wall = rtpp_packet_ext_get_rtime_wall(self->pktxp);
    mono = rtpp_packet_ext_get_rtime_mono(self->pktxp);
    return (Py_BuildValue("(dd)", wall, mono));
}

static int
PyRTPPPacketExt_set_rtime(PyRTPPPacketExtObject *self, PyObject *value,
  void *closure)
{
    PyObject *items;
    PyObject *o0, *o1;
    double wall, mono;

    (void)closure;
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot delete rtime");
        return (-1);
    }
    items = PySequence_Fast(value, "rtime must be a 2-sequence");
    if (items == NULL) {
        return (-1);
    }
    if (PySequence_Fast_GET_SIZE(items) != 2) {
        Py_DECREF(items);
        PyErr_SetString(PyExc_TypeError, "rtime must contain wall and mono");
        return (-1);
    }
    o0 = PySequence_Fast_GET_ITEM(items, 0);
    o1 = PySequence_Fast_GET_ITEM(items, 1);
    wall = PyFloat_AsDouble(o0);
    if (wall == -1.0 && PyErr_Occurred() != NULL) {
        Py_DECREF(items);
        return (-1);
    }
    mono = PyFloat_AsDouble(o1);
    if (mono == -1.0 && PyErr_Occurred() != NULL) {
        Py_DECREF(items);
        return (-1);
    }
    Py_DECREF(items);
    rtpp_packet_ext_set_rtime(self->pktxp, wall, mono);
    return (0);
}

static PyObject *
PyRTPPPacketExt_bytes(PyRTPPPacketExtObject *self, PyObject *args)
{

    (void)args;
    return PyBytes_FromStringAndSize(self->pktxp->data,
      (Py_ssize_t)self->pktxp->dlen);
}

static void
PyRTPPPacketExt_dealloc(PyRTPPPacketExtObject *self)
{
    rtp_packet_ext_dtor(self->pktxp);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
PyRTPPPacketExt_init(PyRTPPPacketExtObject *self, PyObject *args,
  PyObject *kwds)
{
    static char *kwlist[] = {"data", "port", NULL};
    PyObject *dataobj;
    unsigned int port;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OI", kwlist, &dataobj,
      &port)) {
        return (-1);
    }
    assert (self->pktxp == NULL);
    self->pktxp = PyRTPPPacketExt_from_ctor_arg(dataobj, port);
    if (self->pktxp == NULL) {
        return (-1);
    }
    return (0);
}

static PyGetSetDef PyRTPPPacketExt_getset[] = {
    {"port", (getter)PyRTPPPacketExt_get_port, NULL,
     "Packet port", NULL},
    {"rtime", (getter)PyRTPPPacketExt_get_rtime,
     (setter)PyRTPPPacketExt_set_rtime, "Packet receive time", NULL},
    {"_data_ptr", (getter)PyRTPPPacketExt_get__data_ptr, NULL,
     "Address of packet payload data", NULL},
    {NULL}
};

static PyMethodDef PyRTPPPacketExt_methods[] = {
    {"__bytes__", (PyCFunction)PyRTPPPacketExt_bytes, METH_NOARGS,
     "Return packet payload as bytes."},
    {NULL}
};

static PyBufferProcs PyRTPPPacketExt_as_buffer = {
    .bf_getbuffer = PyRTPPPacketExt_getbuffer,
};

static struct RTPPSocket
getRTPPSocket()
{
    int fds[2];
    struct RTPPSocket r = {0};

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto e0;
    }
    PyObject *sock_obj = PyImport_ImportModule("socket");
    if (sock_obj == NULL)
        goto e1;
    PyObject *sock_class = PyObject_GetAttrString(sock_obj, "socket");
    Py_DECREF(sock_obj);
    if (sock_class == NULL)
        goto e1;
    PyObject *fd_arg = PyLong_FromLong(fds[0]);
    if (fd_arg == NULL)
        goto e2;
    PyObject *kwargs = Py_BuildValue("{s:O}", "fileno", fd_arg);
    if (kwargs == NULL)
        goto e3;
    snprintf(r.rtpp_spec, sizeof(r.rtpp_spec), "fd:%d", fds[1]);
    r.py.spec_str = PyUnicode_FromString(r.rtpp_spec);
    if (r.py.spec_str == NULL)
        goto e4;
    r.py.rtpp_sock = PyObject_Call(sock_class, PyTuple_New(0), kwargs);
    if (r.py.rtpp_sock == NULL) {
        goto e5;
    }
    r.fds.our = fds[0];
    r.fds.their = fds[1];
    Py_DECREF(kwargs);
    Py_DECREF(fd_arg);
    Py_DECREF(sock_class);
    return r;
e5:
    Py_DECREF(r.py.spec_str);
e4:
    Py_DECREF(kwargs);
e3:
    Py_DECREF(fd_arg);
e2:
    Py_DECREF(sock_class);
e1:
    close(fds[0]);
    close(fds[1]);
e0:
    return r;
}

static int
arg_count(const char *argv[]) {
    int i;
    for (i = 0; argv[i] != NULL; i++) {}
    return i;
}

static void
arg_append1(const char *argv[], const char *arg) {
    int i = arg_count(argv);
    argv[i++] = arg;
    argv[i] = NULL;
}

static void
arg_append2(const char *argv[], const char *arg, const char *val) {
    int i = arg_count(argv);
    argv[i++] = arg;
    argv[i++] = val;
    argv[i] = NULL;
}

static int
py2c_list(PyObject *list, const char **c_list, int max, const char *name) {
    Py_ssize_t mcount = PySequence_Size(list);
    const char *errf;
    char emsg[256];
    PyObject *erro = PyExc_TypeError;

    if (mcount > max) {
        errf = "Too many %s specified";
        goto e0;
    }
    if (!PySequence_Check(list)) {
        errf = "%s must be a list";
        goto e0;
    }
    int i;
    for (i = 0; i < mcount; i++) {
        PyObject *item = PySequence_GetItem(list, i);
        if (!PyUnicode_Check(item)) {
            Py_XDECREF(item);
            errf = "%s must be a list of strings";
            goto e1;
        }
        c_list[i] = strdup(PyUnicode_AsUTF8(item));  // Does not increase refcount
        Py_DECREF(item);
        if (c_list[i] == NULL) {
            erro = PyExc_MemoryError;
            errf = "Failed to allocate memory for %s names";
            goto e1;
        }
    }
    return 0;
e1:
    for (int j = 0; j < i; j++)
        free((char *)c_list[j]);
e0:
    snprintf(emsg, sizeof(emsg), errf, name);
    PyErr_SetString(erro, emsg);
    return -1;
}

static int PyRTPProxy_init(PyRTPProxyObject* self, PyObject* args, PyObject* kwds) {
    static const char *kwlist[] = {
        "ttl", "setup_ttl", "debug_level", "port_min", "port_max",
        "rec_spool_dir", "rec_final_dir", "modules", "extra_args",
        NULL
    };
    self->rp = RTPPInitializeParams;
    PyObject *modules_obj = NULL;
    PyObject *extra_args_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iisiissOO", (char **)kwlist,
            &self->rp.ttl, &self->rp.setup_ttl, &self->rp.debug_level,
            &self->rp.port_min, &self->rp.port_max, &self->rp.rec_spool_dir,
            &self->rp.rec_final_dir,
            &modules_obj, &extra_args_obj)) {
        goto e0;
    }

    if (modules_obj) {
        if (py2c_list(modules_obj, self->_modules, MAX_MODULES, "modules") != 0) {
            goto e0;
        }
        self->rp.modules = self->_modules;
    }
    if (extra_args_obj) {
        if (py2c_list(extra_args_obj, self->_extra_args, MAX_EXTRA_ARGS, "extra_args") != 0) {
            goto e1;
        }
    }

    self->cmd = getRTPPSocket();
    if (self->cmd.py.rtpp_sock == NULL)
        goto e2;
    self->notify = getRTPPSocket();
    if (self->notify.py.rtpp_sock == NULL)
        goto e3;

    if (self->rp.debug_level != RTPPInitializeParams.debug_level)
        self->rp.debug_level = strdup(self->rp.debug_level);
    if (self->rp.rec_spool_dir != RTPPInitializeParams.rec_spool_dir)
        self->rp.rec_spool_dir = strdup(self->rp.rec_spool_dir);
    if (self->rp.rec_final_dir != RTPPInitializeParams.rec_final_dir)
        self->rp.rec_final_dir = strdup(self->rp.rec_final_dir);
    if (self->rp.debug_level == NULL || self->rp.rec_spool_dir == NULL ||
      self->rp.rec_final_dir == NULL) {
        PyErr_SetString(PyExc_ValueError, "Failed to allocate memory for module values");
        goto e4;
    }
    const char *argv[256] = {
       "rtpproxy",
       "-s", self->cmd.rtpp_spec,
       "-d", self->rp.debug_level,
       "-n", self->notify.rtpp_spec,
       "-S", self->rp.rec_spool_dir,
       "-r", self->rp.rec_final_dir,
    };
    if (self->rp.ttl >= 0) {
        snprintf(self->ttl_val, sizeof(self->ttl_val), "%d", self->rp.ttl);
        arg_append2(argv, "-T", self->ttl_val);
    }
    if (self->rp.setup_ttl >= 0) {
        snprintf(self->setup_ttl_val, sizeof(self->setup_ttl_val), "%d",
          self->rp.setup_ttl);
        arg_append2(argv, "-W", self->setup_ttl_val);
    }
    for (int i = 0; i < MAX_MODULES; i++) {
        if (self->rp.modules[i] == NULL)
            break;
        arg_append2(argv, "--dso", self->rp.modules[i]);
    }
    if (self->rp.port_min > 0) {
        snprintf(self->port_min, sizeof(self->port_min), "%d", self->rp.port_min);
        arg_append2(argv, "-m", self->port_min);
    }
    if (self->rp.port_max > 0) {
        snprintf(self->port_max, sizeof(self->port_max), "%d", self->rp.port_max);
        arg_append2(argv, "-M", self->port_max);
    }
    for (int i = 0; i < MAX_EXTRA_ARGS; i++) {
        if (self->_extra_args[i] == NULL)
            break;
        arg_append1(argv, self->_extra_args[i]);
    }
    int argc = arg_count(argv);

    self->rtpp_ctx = rtpp_main(argc, argv);
    if(self->rtpp_ctx == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Error initializing RTPProxy instance");
        goto e4;
    }

    return 0;
e4:
    if (self->rp.debug_level != RTPPInitializeParams.debug_level)
        free((char *)self->rp.debug_level);
    if (self->rp.rec_spool_dir != RTPPInitializeParams.rec_spool_dir)
        free((char *)self->rp.rec_spool_dir);
    if (self->rp.rec_final_dir != RTPPInitializeParams.rec_final_dir)
        free((char *)self->rp.rec_final_dir);
    close(self->notify.fds.their);
    Py_DECREF(self->notify.py.rtpp_sock);
    Py_DECREF(self->notify.py.spec_str);
e3:
    close(self->cmd.fds.their);
    Py_DECREF(self->cmd.py.rtpp_sock);
    Py_DECREF(self->cmd.py.spec_str);
e2:
    for (int i = 0; i < MAX_EXTRA_ARGS && self->_extra_args[i] != NULL; i++)
        free((char *)self->_extra_args[i]);
e1:
    for (int i = 0; i < MAX_MODULES && self->_modules[i] != NULL; i++)
        free((char *)self->_modules[i]);
e0:
    return -1;
}

// The __del__ method for PyRTPProxy objects
static void PyRTPProxy_dealloc(PyRTPProxyObject* self) {
    if (self->rtpp_ctx != NULL) {
        rtpp_shutdown(self->rtpp_ctx);
        Py_DECREF(self->cmd.py.rtpp_sock);
        Py_DECREF(self->cmd.py.spec_str);
        close(self->cmd.fds.their);
        Py_DECREF(self->notify.py.rtpp_sock);
        Py_DECREF(self->notify.py.spec_str);
        close(self->notify.fds.their);
        if (self->rp.debug_level != RTPPInitializeParams.debug_level)
            free((char *)self->rp.debug_level);
        if (self->rp.rec_spool_dir != RTPPInitializeParams.rec_spool_dir)
            free((char *)self->rp.rec_spool_dir);
        if (self->rp.rec_final_dir != RTPPInitializeParams.rec_final_dir)
            free((char *)self->rp.rec_final_dir);
        for (int i = 0; i < MAX_MODULES && self->_modules[i] != NULL; i++) {
            free((char *)self->_modules[i]);
        }
    }
}

static void
PyRTPPPacketPort_dealloc(PyRTPPPacketPortObject *self)
{
    free(self->push_batch);
    self->push_batch = NULL;
    free(self->pop_batch);
    self->pop_batch = NULL;
    if (self->packetport != NULL) {
        rtpp_packetport_dtor(self->packetport);
        self->packetport = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
PyRTPProxy_packetport_ctor(PyRTPProxyObject *self, PyObject *args,
  PyObject *kwds)
{
    static char *kwlist[] = {"capacity", "flush_mode", "flush_interval", NULL};
    struct rtpp_packetport_ctor_args ppca;
    struct rtpp_packetport *packetport;
    const char *flush_mode = "explicit";
    double flush_interval = 0.01;
    unsigned int capacity;

    (void)self;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "I|sd", kwlist, &capacity,
      &flush_mode, &flush_interval)) {
        return NULL;
    }
    if (capacity == 0) {
        PyErr_SetString(PyExc_ValueError, "capacity must be greater than 0");
        return NULL;
    }
    if (flush_interval < 0.0) {
        PyErr_SetString(PyExc_ValueError,
          "flush_interval must not be negative");
        return NULL;
    }
    ppca = (struct rtpp_packetport_ctor_args){
        .capacity = capacity,
        .flush_mode = RTPP_PACKETPORT_FLUSH_TIMER,
    };
    if (strcmp(flush_mode, "timer") == 0) {
        ppca.flush_mode = RTPP_PACKETPORT_FLUSH_TIMER;
    } else if (strcmp(flush_mode, "explicit") == 0) {
        ppca.flush_mode = RTPP_PACKETPORT_FLUSH_EXPLICIT;
    } else {
        PyErr_SetString(PyExc_ValueError,
          "flush_mode must be 'timer' or 'explicit'");
        return NULL;
    }
    ppca.flush_interval.tv_sec = (time_t)flush_interval;
    ppca.flush_interval.tv_nsec = (long)((flush_interval -
      (double)ppca.flush_interval.tv_sec) * 1000000000.0);
    if (ppca.flush_interval.tv_nsec >= 1000000000L) {
        ppca.flush_interval.tv_sec += 1;
        ppca.flush_interval.tv_nsec -= 1000000000L;
    }
    packetport = rtpp_packetport_ctor(&ppca);
    if (packetport == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "rtpp_packetport_ctor() failed");
        return NULL;
    }
    return PyRTPPPacketPort_FromRTPP(packetport, capacity);
}

static PyMethodDef PyRTPProxy_methods[] = {
    {"packetport_ctor", (PyCFunction)PyRTPProxy_packetport_ctor,
     METH_VARARGS | METH_KEYWORDS,
     "Create an rtpp_packetport wrapper."},
    {NULL}  // Sentinel
};

static PyMemberDef PyRTPProxy_members[] = {
    {"rtpp_sock", T_OBJECT_EX, offsetof(PyRTPProxyObject, cmd.py.rtpp_sock),
     READONLY, "RTPProxy command socket"},
    {"rtpp_nsock", T_OBJECT_EX, offsetof(PyRTPProxyObject, notify.py.rtpp_sock),
     READONLY, "RTPProxy notification socket"},
    {"rtpp_sock_spec", T_OBJECT_EX, offsetof(PyRTPProxyObject, cmd.py.spec_str),
     READONLY, "RTPProxy command socket specifier"},
    {"rtpp_nsock_spec", T_OBJECT_EX, offsetof(PyRTPProxyObject, notify.py.spec_str),
     READONLY, "RTPProxy notification socket specifier"},
    {NULL}
};

static PyMemberDef PyRTPPPacketPort_members[] = {
    {"capacity", T_UINT, offsetof(PyRTPPPacketPortObject, capacity),
     READONLY, "Packetport queue capacity"},
    {NULL}
};

static PyMethodDef PyRTPPPacketPort_methods[] = {
    {"address_ref", (PyCFunction)PyRTPPPacketPort_address_ref, METH_NOARGS,
     "Return packetport address reference in RTQ form."},
    {"next_in_port", (PyCFunction)PyRTPPPacketPort_next_in_port, METH_NOARGS,
     "Allocate the next public packetport number."},
    {"push", (PyCFunction)PyRTPPPacketPort_push, METH_VARARGS,
     "Push bytes plus port, or consume an rtp.io.packet."},
    {"try_push", (PyCFunction)PyRTPPPacketPort_try_push, METH_VARARGS,
     "Try to push bytes plus port, or a packet, without consuming it on failure."},
    {"try_push_many", (PyCFunction)PyRTPPPacketPort_try_push_many, METH_O,
     "Try to push a sequence of packets and return the number consumed."},
    {"try_pop", (PyCFunction)PyRTPPPacketPort_try_pop, METH_NOARGS,
     "Try to pop a packet payload from the packetport."},
    {"try_pop_many", (PyCFunction)PyRTPPPacketPort_try_pop_many, METH_VARARGS,
     "Try to pop up to max_items packet payloads from the packetport."},
    {"flush", (PyCFunction)PyRTPPPacketPort_flush, METH_NOARGS,
     "Request immediate packetport outbound processing."},
    {NULL}
};

static PyTypeObject PyRTPProxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME_STR "." CLS_NAME_STR,
    .tp_doc = "Module to run RTPProxy inside Python.",
    .tp_basicsize = sizeof(PyRTPProxyObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)PyRTPProxy_init,
    .tp_dealloc = (destructor)PyRTPProxy_dealloc,
    .tp_methods = PyRTPProxy_methods,
    .tp_members = PyRTPProxy_members,
};

static PyTypeObject PyRTPPPacketPortType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME_STR ".packetport",
    .tp_doc = "Wrapper for rtpp_packetport.",
    .tp_basicsize = sizeof(PyRTPPPacketPortObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)PyRTPPPacketPort_dealloc,
    .tp_methods = PyRTPPPacketPort_methods,
    .tp_members = PyRTPPPacketPort_members,
};

static PyTypeObject PyRTPPPacketExtType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME_STR ".packet",
    .tp_doc = "Wrapper for rtp_packet_ext.",
    .tp_basicsize = sizeof(PyRTPPPacketExtObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)PyRTPPPacketExt_init,
    .tp_dealloc = (destructor)PyRTPPPacketExt_dealloc,
    .tp_methods = PyRTPPPacketExt_methods,
    .tp_getset = PyRTPPPacketExt_getset,
    .tp_as_buffer = &PyRTPPPacketExt_as_buffer,
};

static struct PyModuleDef RTPProxy_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = MODULE_NAME_STR,
    .m_doc = "Module to run RTPProxy inside Python.",
    .m_size = -1,
};

// Module initialization function
PyMODINIT_FUNC PY_INIT_FUNC(void) {
    PyObject* module;
    if (PyType_Ready(&PyRTPProxyType) < 0)
        return NULL;
    if (PyType_Ready(&PyRTPPPacketPortType) < 0)
        return NULL;
    if (PyType_Ready(&PyRTPPPacketExtType) < 0)
        return NULL;

    module = PyModule_Create(&RTPProxy_module);
    if (module == NULL)
        return NULL;

    Py_INCREF(&PyRTPProxyType);
    PyModule_AddObject(module, CLS_NAME_STR, (PyObject*)&PyRTPProxyType);
    Py_INCREF(&PyRTPPPacketPortType);
    PyModule_AddObject(module, "packetport", (PyObject *)&PyRTPPPacketPortType);
    Py_INCREF(&PyRTPPPacketExtType);
    PyModule_AddObject(module, "packet", (PyObject *)&PyRTPPPacketExtType);

    return module;
}
