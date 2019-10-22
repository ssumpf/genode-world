
// Gendoe includes
#include <base/attached_rom_dataspace.h>
#include <base/component.h>
#include <base/heap.h>
#include <base/signal.h>
#include <os/reporter.h>
#include <util/reconstructible.h>
#include <util/string.h>

// local includes
#include "terminal_root.h"


namespace Controller {

	using namespace Genode;

	class Main;
}

class Controller::Main
{
	private:

		Env&                       _env;
		Heap                       _heap                       { _env.ram(), _env.rm() };

		Signal_handler<Main>       _terminal_data_handler      { _env.ep(), *this,
		                                                         &Main::_handle_terminal_data };
		Signal_handler<Main>       _terminal_connected_handler { _env.ep(), *this,
		                                                         &Main::_handle_terminal_connected };
		Terminal_root              _terminal_root              { _env, _heap,
		                                                         _terminal_connected_handler,
		                                                         _terminal_data_handler };

		void _handle_terminal_connected();
		void _handle_terminal_data();

	public:

		Main(Env &env) : _env(env) {
			_env.parent().announce(_env.ep().manage(_terminal_root));
		}

		~Main() = default;
};


void Controller::Main::_handle_terminal_connected()
{
	String<50> temp { "req uest;" };
	_terminal_root.current_session()->write(temp.string(), temp.length());
}


void Controller::Main::_handle_terminal_data()
{
	char                tmp[100];
	const size_t        cnt       { _terminal_root.current_session()->read(tmp, 50) };
	Genode::String<120> data      { Genode::Cstring { tmp, cnt } };

	if (cnt == 0) return;

	Genode::warning("data received >> count=",cnt,"  data='",data,"'");

	log("Test finished");
}


void Component::construct(Genode::Env& env) { static Controller::Main main(env); }
