/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "device.h"
#include "memlock.h"
#include "defaults.h"

#include <stdarg.h>
#include <syslog.h>

static FILE *_log_file;
static struct device _log_dev;
static struct dm_str_list _log_dev_alias;

static int _syslog = 0;
static int _log_to_file = 0;
static int _log_direct = 0;
static int _log_while_suspended = 0;
static int _indent = 1;
static int _log_suppress = 0;
static char _msg_prefix[30] = "  ";
static int _already_logging = 0;
static int _abort_on_internal_errors = 0;

static lvm2_log_fn_t _lvm2_log_fn = NULL;

static int _lvm_errno = 0;
static int _store_errmsg = 0;
static char *_lvm_errmsg = NULL;
static size_t _lvm_errmsg_size = 0;
static size_t _lvm_errmsg_len = 0;
#define MAX_ERRMSG_LEN (512 * 1024)  /* Max size of error buffer 512KB */

void init_log_fn(lvm2_log_fn_t log_fn)
{
	if (log_fn)
		_lvm2_log_fn = log_fn;
	else
		_lvm2_log_fn = NULL;
}

void init_log_file(const char *log_file, int append)
{
	const char *open_mode = append ? "a" : "w";

	if (!(_log_file = fopen(log_file, open_mode))) {
		log_sys_error("fopen", log_file);
		return;
	}

	_log_to_file = 1;
}

void init_log_direct(const char *log_file, int append)
{
	int open_flags = append ? 0 : O_TRUNC;

	dev_create_file(log_file, &_log_dev, &_log_dev_alias, 1);
	if (!dev_open_flags(&_log_dev, O_RDWR | O_CREAT | open_flags, 1, 0))
		return;

	_log_direct = 1;
}

void init_log_while_suspended(int log_while_suspended)
{
	_log_while_suspended = log_while_suspended;
}

void init_syslog(int facility)
{
	openlog("lvm", LOG_PID, facility);
	_syslog = 1;
}

int log_suppress(int suppress)
{
	int old_suppress = _log_suppress;

	_log_suppress = suppress;

	return old_suppress;
}

void release_log_memory(void)
{
	if (!_log_direct)
		return;

	dm_free((char *) _log_dev_alias.str);
	_log_dev_alias.str = "activate_log file";
}

void fin_log(void)
{
	if (_log_direct) {
		(void) dev_close(&_log_dev);
		_log_direct = 0;
	}

	if (_log_to_file) {
		if (dm_fclose(_log_file)) {
			if (errno)
			      fprintf(stderr, "failed to write log file: %s\n",
				      strerror(errno));
			else
			      fprintf(stderr, "failed to write log file\n");

		}
		_log_to_file = 0;
	}
}

void fin_syslog(void)
{
	if (_syslog)
		closelog();
	_syslog = 0;
}

void init_msg_prefix(const char *prefix)
{
	strncpy(_msg_prefix, prefix, sizeof(_msg_prefix) - 1);
	_msg_prefix[sizeof(_msg_prefix) - 1] = '\0';
}

void init_indent(int indent)
{
	_indent = indent;
}

void init_abort_on_internal_errors(int fatal)
{
	_abort_on_internal_errors = fatal;
}

void reset_lvm_errno(int store_errmsg)
{
	_lvm_errno = 0;

	if (_lvm_errmsg) {
		dm_free(_lvm_errmsg);
		_lvm_errmsg = NULL;
		_lvm_errmsg_size = _lvm_errmsg_len = 0;
	}

	_store_errmsg = store_errmsg;
}

int stored_errno(void)
{
	return _lvm_errno;
}

const char *stored_errmsg(void)
{
	return _lvm_errmsg ? : "";
}

const char *stored_errmsg_with_clear(void)
{
	const char *rc = strdup(stored_errmsg());
	reset_lvm_errno(1);
	return rc;
}

static struct dm_hash_table *_duplicated = NULL;

void reset_log_duplicated(void) {
	if (_duplicated) {
		dm_hash_destroy(_duplicated);
		_duplicated = NULL;
	}
}

void print_log(int level, const char *file, int line, int dm_errno_or_class,
	       const char *format, ...)
{
	va_list ap;
	char buf[1024], locn[4096];
	int bufused, n;
	const char *message;
	const char *trformat;		/* Translated format string */
	char *newbuf;
	int use_stderr = level & _LOG_STDERR;
	int log_once = level & _LOG_ONCE;
	int fatal_internal_error = 0;
	size_t msglen;
	const char *indent_spaces = "";
	FILE *stream;

	level &= ~(_LOG_STDERR|_LOG_ONCE);

	if (_abort_on_internal_errors &&
	    !strncmp(format, INTERNAL_ERROR, sizeof(INTERNAL_ERROR) - 1)) {
		fatal_internal_error = 1;
		/* Internal errors triggering abort cannot be suppressed. */
		_log_suppress = 0;
		level = _LOG_FATAL;
	}

	if (_log_suppress == 2)
		return;

	if (level <= _LOG_ERR)
		init_error_message_produced(1);

	trformat = _(format);

	if (level < _LOG_DEBUG && dm_errno_or_class && !_lvm_errno)
		_lvm_errno = dm_errno_or_class;

	if (_lvm2_log_fn ||
	    (_store_errmsg && (level <= _LOG_ERR)) ||
	    log_once) {
		va_start(ap, format);
		n = vsnprintf(locn, sizeof(locn) - 1, trformat, ap);
		va_end(ap);

		if (n < 0) {
			fprintf(stderr, _("vsnprintf failed: skipping external "
					"logging function"));
			goto log_it;
		}

		locn[sizeof(locn) - 1] = '\0';
		message = locn;
	}

/* FIXME Avoid pointless use of message buffer when it'll never be read! */
	if (_store_errmsg && (level <= _LOG_ERR) &&
	    _lvm_errmsg_len < MAX_ERRMSG_LEN) {
		msglen = strlen(message);
		if ((_lvm_errmsg_len + msglen + 1) >= _lvm_errmsg_size) {
			_lvm_errmsg_size = 2 * (_lvm_errmsg_len + msglen + 1);
			if ((newbuf = dm_realloc(_lvm_errmsg,
						 _lvm_errmsg_size)))
				_lvm_errmsg = newbuf;
			else
				_lvm_errmsg_size = _lvm_errmsg_len;
		}
		if (_lvm_errmsg &&
		    (_lvm_errmsg_len + msglen + 2) < _lvm_errmsg_size) {
			/* prepend '\n' and copy with '\0' but do not count in */
                        if (_lvm_errmsg_len)
				_lvm_errmsg[_lvm_errmsg_len++] = '\n';
			memcpy(_lvm_errmsg + _lvm_errmsg_len, message, msglen + 1);
			_lvm_errmsg_len += msglen;
		}
	}

	if (log_once) {
		if (!_duplicated)
			_duplicated = dm_hash_create(128);
		if (_duplicated) {
			if (dm_hash_lookup(_duplicated, message))
				level = _LOG_NOTICE;
			else
				(void) dm_hash_insert(_duplicated, message, (void*)1);
		}
	}

	if (_lvm2_log_fn) {
		_lvm2_log_fn(level, file, line, 0, message);
		if (fatal_internal_error)
			abort();
		return;
	}

      log_it:
	if ((verbose_level() >= level) && !_log_suppress) {
		if (verbose_level() > _LOG_DEBUG) {
			(void) dm_snprintf(buf, sizeof(buf), "#%s:%d ",
					   file, line);
		} else
			buf[0] = '\0';

		if (_indent)
			switch (level) {
			case _LOG_NOTICE: indent_spaces = "  "; break;
			case _LOG_INFO:   indent_spaces = "    "; break;
			case _LOG_DEBUG:  indent_spaces = "      "; break;
			default: /* nothing to do */;
			}

		va_start(ap, format);
		switch (level) {
		case _LOG_DEBUG:
			if ((verbose_level() == level) &&
			    (strcmp("<backtrace>", format) == 0))
				break;
			if (verbose_level() < _LOG_DEBUG)
				break;
			if (!debug_class_is_logged(dm_errno_or_class))
				break;
			/* fall through */
		default:
			/* Typically only log_warn goes to stdout */
			stream = (use_stderr || (level != _LOG_WARN)) ? stderr : stdout;
			fprintf(stream, "%s%s%s%s", buf, log_command_name(),
				_msg_prefix, indent_spaces);
			vfprintf(stream, trformat, ap);
			fputc('\n', stream);
		}
		va_end(ap);
	}

	if ((level > debug_level()) ||
	    (level >= _LOG_DEBUG && !debug_class_is_logged(dm_errno_or_class))) {
		if (fatal_internal_error)
			abort();
		return;
	}

	if (_log_to_file && (_log_while_suspended || !critical_section())) {
		fprintf(_log_file, "%s:%d %s%s", file, line, log_command_name(),
			_msg_prefix);

		va_start(ap, format);
		vfprintf(_log_file, trformat, ap);
		va_end(ap);

		fprintf(_log_file, "\n");
		fflush(_log_file);
	}

	if (_syslog && (_log_while_suspended || !critical_section())) {
		va_start(ap, format);
		vsyslog(level, trformat, ap);
		va_end(ap);
	}

	if (fatal_internal_error)
		abort();

	/* FIXME This code is unfinished - pre-extend & condense. */
	if (!_already_logging && _log_direct && critical_section()) {
		_already_logging = 1;
		memset(&buf, ' ', sizeof(buf));
		bufused = 0;
		if ((n = dm_snprintf(buf, sizeof(buf) - 1,
				      "%s:%d %s%s", file, line, log_command_name(),
				      _msg_prefix)) == -1)
			goto done;

		bufused += n;

		va_start(ap, format);
		n = vsnprintf(buf + bufused - 1, sizeof(buf) - bufused - 1,
			      trformat, ap);
		va_end(ap);
		bufused += n;

		buf[bufused - 1] = '\n';
	      done:
		buf[bufused] = '\n';
		buf[sizeof(buf) - 1] = '\n';
		/* FIXME real size bufused */
		dev_append(&_log_dev, sizeof(buf), buf);
		_already_logging = 0;
	}
}
