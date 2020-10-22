/*
 * \brief  Entry point for POSIX applications
 * \author Norman Feske
 * \date   2016-12-23
 */

/*
 * Copyright (C) 2016-2020 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <libc/component.h>

/* libc includes */
#include <libc/args.h>
#include <stdlib.h> /* 'exit'   */

/* initial environment for the FreeBSD libc implementation */
extern char **environ;

/* provided by the application */
extern "C" int main(int argc, char **argv, char **envp);

extern "C" unsigned char __bss_start;
extern "C" unsigned char _prog_img_end;

extern "C" void wait_for_continue();

static void clear_bss()
{
	Genode::log("CLEAR: ", &__bss_start, " size: ", &_prog_img_end - &__bss_start - 4);
	Genode::memset(&__bss_start, 0, &_prog_img_end - &__bss_start - 4);
}
static void construct_component(Libc::Env &env)
{
	int argc    = 5;
	char **envp = nullptr;

	enum { SEQ_COUNT = 5 };

//	populate_args_and_env(env, argc, argv, envp);
	char const *arg[SEQ_COUNT][6] =  {
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--enter-pin=1889", "HOME=/" },
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--query-registration-state", "HOME=/" },
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--attach-packet-service", "HOME=/" },
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--connect=apn='internet.eplus.de',auth='PAP',username='eplus',password='eplus'", "HOME=/" },
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--disconnect", "HOME=/" }
	};

	int ret = 0;
	Genode::log("BSS: ", &__bss_start, " - ", &_prog_img_end);
	for (unsigned i = 1; i < SEQ_COUNT /*SEQ_COUNT*/; i++) {
		envp = (char **)&arg[i][5];
		environ = envp;

		Genode::warning("call main ", i);
		ret = main(argc, (char **)arg[i], envp);
		Genode::warning("returned ", ret, " i: ", i);
		clear_bss();
	}

	Genode::warning("EXIT: ", ret);
	exit(ret);
}


void Libc::Component::construct(Libc::Env &env)
{
	Libc::with_libc([&] () { construct_component(env); });
}
