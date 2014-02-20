/*
 * brickd
 * Copyright (C) 2012-2014 Matthias Bolte <matthias@tinkerforge.com>
 *
 * iokit.c: IOKit specific functions
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <errno.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <AvailabilityMacros.h>
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
	#include <objc/objc-auto.h>
#endif

#include "iokit.h"

#include "event.h"
#include "log.h"
#include "pipe.h"
#include "threads.h"
#include "usb.h"
#include "utils.h"

#define LOG_CATEGORY LOG_CATEGORY_OTHER

static Pipe _notification_pipe;
static Thread _poller_thread;
static int _running = 0;
static CFRunLoopRef _run_loop = NULL;

static void iokit_forward_notifications(void *opaque) {
	uint8_t byte;

	(void)opaque;

	if (pipe_read(&_notification_pipe, &byte, sizeof(byte)) < 0) {
		log_error("Could not read from notification pipe: %s (%d)",
		          get_errno_name(errno), errno);

		return;
	}

	usb_reopen();
}

static void iokit_handle_notifications(void *opaque, io_service_t service,
                                       uint32_t message_type, void *message_argument) {
	io_connect_t *root_port = opaque;
	uint8_t byte;

	(void)service;

	switch (message_type) {
	case kIOMessageCanSystemSleep:
		IOAllowPowerChange(*root_port, (long)message_argument);

		break;

	case kIOMessageSystemWillSleep:
		IOAllowPowerChange(*root_port, (long)message_argument);

		log_debug("Received IOKit sleep notification");

		break;

	case kIOMessageSystemWillPowerOn:
		break;

	case kIOMessageSystemHasPoweredOn:
		log_debug("Received IOKit wakeup notification");

		if (pipe_write(&_notification_pipe, &byte, sizeof(byte)) < 0) {
			log_error("Could not write to notification pipe: %s (%d)",
			          get_errno_name(errno), errno);
		}

		break;

	default:
		break;
	}
}

static void iokit_poll_notifications(void *opaque) {
	int phase = 0;
	Semaphore *handshake = opaque;
	IONotificationPortRef notification_port;
	io_object_t notifier;
	io_connect_t root_port;
	CFRunLoopSourceRef notification_run_loop_source;

	log_debug("Started notification poll thread");

	// need to register this PThread with the Objective-C garbage collector,
	// because CoreFoundation uses Objective-C
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
	objc_registerThreadWithCollector();
#endif

	// register for system sleep/wake notifications
	root_port = IORegisterForSystemPower(&root_port, &notification_port,
	                                     iokit_handle_notifications, &notifier);

	if (root_port == MACH_PORT_NULL) {
		log_error("Could not register for root power domain");

		goto cleanup;
	}

	phase = 1;

	// get notification run loop source
	notification_run_loop_source = IONotificationPortGetRunLoopSource(notification_port);

	if (notification_run_loop_source == NULL) {
		log_error("Could not get notification run loop source");

		goto cleanup;
	}

	CFRunLoopAddSource(CFRunLoopGetCurrent(), notification_run_loop_source,
	                   kCFRunLoopDefaultMode);

	phase = 2;

	// start loop
	_run_loop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());

	_running = 1;
	semaphore_release(handshake);

	CFRunLoopRun();

	log_debug("Stopped notification poll thread");

cleanup:
	if (!_running) {
		// need to release the handshake in all cases, otherwise iokit_init
		// will block forever in semaphore_acquire
		semaphore_release(handshake);
	}

	switch (phase) { // no breaks, all cases fall through intentionally
	case 2:
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), notification_run_loop_source,
		                      kCFRunLoopDefaultMode);

	case 1:
		IODeregisterForSystemPower(&notifier);
		IOServiceClose(root_port);
		IONotificationPortDestroy(notification_port);

	default:
		break;
	}

	_running = 0;
}

int iokit_init(void) {
	int phase = 0;
	Semaphore handshake;

	log_debug("Initializing IOKit subsystem");

	// create notification pipe
	if (pipe_create(&_notification_pipe) < 0) {
		log_error("Could not create notification pipe: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 1;

	if (event_add_source(_notification_pipe.read_end, EVENT_SOURCE_TYPE_GENERIC,
	                     EVENT_READ, iokit_forward_notifications, NULL) < 0) {
		goto cleanup;
	}

	phase = 2;

	// create notification poll thread
	if (semaphore_create(&handshake) < 0) {
		log_error("Could not create handshake semaphore: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	thread_create(&_poller_thread, iokit_poll_notifications, &handshake);

	semaphore_acquire(&handshake);
	semaphore_destroy(&handshake);

	phase = 3;

	if (!_running) {
		log_error("Could not start notification poll thread");

		goto cleanup;
	}

	phase = 4;

cleanup:
	switch (phase) { // no breaks, all cases fall through intentionally
	case 3:
		thread_destroy(&_poller_thread);

	case 2:
		event_remove_source(_notification_pipe.read_end, EVENT_SOURCE_TYPE_GENERIC);

	case 1:
		pipe_destroy(&_notification_pipe);

	default:
		break;
	}

	return phase == 4 ? 0 : -1;
}

void iokit_exit(void) {
	log_debug("Shutting down IOKit subsystem");

	if (_running) {
		_running = 0;

		CFRunLoopStop(_run_loop);
		CFRelease(_run_loop);

		thread_join(&_poller_thread);
	}

	thread_destroy(&_poller_thread);

	event_remove_source(_notification_pipe.read_end, EVENT_SOURCE_TYPE_GENERIC);

	pipe_destroy(&_notification_pipe);
}