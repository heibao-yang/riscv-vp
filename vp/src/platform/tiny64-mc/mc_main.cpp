#include <boost/io/ios_state.hpp>
#include <boost/program_options.hpp>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>

#include "core/common/clint.h"
#include "elf_loader.h"
#include "gdb-mc/gdb_runner.h"
#include "gdb-mc/gdb_server.h"
#include "iss.h"
#include "mem.h"
#include "memory.h"
#include "syscall.h"

using namespace rv64;

struct Options {
	typedef unsigned int addr_t;

	Options &check_and_post_process() {
		return *this;
	}

	std::string input_program;

	addr_t mem_size = 1024 * 1024 * 32;  // 32 MB ram, to place it before the CLINT and run the base examples (assume
	                                     // memory start at zero) without modifications
	addr_t mem_start_addr = 0x00000000;
	addr_t mem_end_addr = mem_start_addr + mem_size - 1;
	addr_t clint_start_addr = 0x02000000;
	addr_t clint_end_addr = 0x0200ffff;
	addr_t sys_start_addr = 0x02010000;
	addr_t sys_end_addr = 0x020103ff;

	bool use_debug_runner = false;
	unsigned int debug_port = 5005;

	bool use_instr_dmi = false;
	bool use_data_dmi = false;
	bool trace_mode = false;
	bool intercept_syscalls = false;
	bool quiet = false;

	unsigned int tlm_global_quantum = 10;
};

Options parse_command_line_arguments(int argc, char **argv) {
	// Note: first check for *help* argument then run *notify*, see:
	// https://stackoverflow.com/questions/5395503/required-and-optional-arguments-using-boost-library-program-options
	try {
		Options opt;

		namespace po = boost::program_options;

		po::options_description desc("Options");

		// clang-format off
		desc.add_options()
		("help", "produce help message")
		("quiet", po::bool_switch(&opt.quiet), "do not output register values on exit")
		("memory-start", po::value<unsigned int>(&opt.mem_start_addr),"set memory start address")
		("trace-mode", po::bool_switch(&opt.trace_mode), "enable instruction tracing")
		("intercept-syscalls", po::bool_switch(&opt.intercept_syscalls),"directly intercept and handle syscalls in the ISS")
		("debug-mode", po::bool_switch(&opt.use_debug_runner),"start execution in debugger (using gdb rsp interface)")
		("debug-port", po::value<unsigned int>(&opt.debug_port), "select port number to connect with GDB")
		("tlm-global-quantum", po::value<unsigned int>(&opt.tlm_global_quantum), "set global tlm quantum (in NS)")
		("use-instr-dmi", po::bool_switch(&opt.use_instr_dmi), "use dmi to fetch instructions")
		("use-data-dmi", po::bool_switch(&opt.use_data_dmi), "use dmi to execute load/store operations")
		("use-dmi", po::bool_switch(), "use instr and data dmi")
		("input-file", po::value<std::string>(&opt.input_program)->required(), "input file to use for execution");
		// clang-format on

		po::positional_options_description pos;
		pos.add("input-file", 1);

		po::variables_map vm;
		po::store(po::command_line_parser(argc, argv).options(desc).positional(pos).run(), vm);

		if (vm.count("help")) {
			std::cout << desc << std::endl;
			exit(0);
		}

		po::notify(vm);

		if (vm["use-dmi"].as<bool>()) {
			opt.use_data_dmi = true;
			opt.use_instr_dmi = true;
		}

		return opt.check_and_post_process();
	} catch (boost::program_options::error &e) {
		std::cerr << "Error parsing command line options: " << e.what() << std::endl;
		exit(-1);
	}
}

int sc_main(int argc, char **argv) {
	Options opt = parse_command_line_arguments(argc, argv);

	std::srand(std::time(nullptr));  // use current time as seed for random generator

	tlm::tlm_global_quantum::instance().set(sc_core::sc_time(opt.tlm_global_quantum, sc_core::SC_NS));

	ISS core0(0);
	MMU mmu0(core0);
	ISS core1(1);
	MMU mmu1(core1);

	CombinedMemoryInterface core0_mem_if("MemoryInterface0", core0, mmu0);
	CombinedMemoryInterface core1_mem_if("MemoryInterface1", core1, mmu1);

	SimpleMemory mem("SimpleMemory", opt.mem_size);
	ELFLoader loader(opt.input_program.c_str());
	SimpleBus<3, 3> bus("SimpleBus");
	SyscallHandler sys("SyscallHandler");
	CLINT<2> clint("CLINT");
	DebugMemoryInterface dbg_if("DebugMemoryInterface");

	std::shared_ptr<BusLock> bus_lock = std::make_shared<BusLock>();
	core0_mem_if.bus_lock = bus_lock;
	core1_mem_if.bus_lock = bus_lock;

	bus.ports[0] = new PortMapping(opt.mem_start_addr, opt.mem_end_addr);
	bus.ports[1] = new PortMapping(opt.clint_start_addr, opt.clint_end_addr);
	bus.ports[2] = new PortMapping(opt.sys_start_addr, opt.sys_end_addr);

	loader.load_executable_image(mem.data, mem.size, opt.mem_start_addr);

	core0.init(&core0_mem_if, &core0_mem_if, &clint, loader.get_entrypoint(),
	           opt.mem_end_addr - 3);  // -3 to not overlap with the next region and stay 32 bit aligned
	core1.init(&core1_mem_if, &core1_mem_if, &clint, loader.get_entrypoint(), opt.mem_end_addr - 32767);

	sys.init(mem.data, opt.mem_start_addr, loader.get_heap_addr());
	sys.register_core(&core0);
	sys.register_core(&core1);

	if (opt.intercept_syscalls) {
		core0.sys = &sys;
		core1.sys = &sys;
	}

	// connect TLM sockets
	core0_mem_if.isock.bind(bus.tsocks[0]);
	core1_mem_if.isock.bind(bus.tsocks[1]);
	dbg_if.isock.bind(bus.tsocks[2]);
	bus.isocks[0].bind(mem.tsock);
	bus.isocks[1].bind(clint.tsock);
	bus.isocks[2].bind(sys.tsock);

	// connect interrupt signals/communication
	clint.target_harts[0] = &core0;
	clint.target_harts[1] = &core1;

	// switch for printing instructions
	core0.trace = opt.trace_mode;
	core1.trace = opt.trace_mode;

	std::vector<debug_target_if *> threads;
	threads.push_back(&core0);
	threads.push_back(&core1);

	if (opt.use_debug_runner) {
		auto server = new GDBServer("GDBServer", threads, &dbg_if, opt.debug_port);
		new GDBServerRunner("GDBRunner0", server, &core0);
		new GDBServerRunner("GDBRunner1", server, &core1);
	} else {
		new DirectCoreRunner(core0);
		new DirectCoreRunner(core1);
	}

	if (opt.quiet)
		sc_core::sc_report_handler::set_verbosity_level(sc_core::SC_NONE);

	sc_core::sc_start();
	if (!opt.quiet) {
		core0.show();
		core1.show();
	}

	return 0;
}
