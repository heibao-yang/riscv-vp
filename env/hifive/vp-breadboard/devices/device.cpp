/*
 * device.cpp
 *
 *  Created on: Sep 30, 2021
 *      Author: dwd
 */

#include "device.hpp"

using std::string;
using std::cout;
using std::cerr;
using std::endl;
using luabridge::LuaRef;
using luabridge::LuaResult;


Device::Device(string id, LuaRef env) : id(id), env(env){
	if(PIN_Interface::implementsInterface(env)) {
		pin = std::make_unique<PIN_Interface>(env);
	}
	if(SPI_Interface::implementsInterface(env)) {
		spi = std::make_unique<SPI_Interface>(env);
	}
	if(Config_Interface::implementsInterface(env)) {
		conf = std::make_unique<Config_Interface>(env);
	}
};

const string& Device::getID() const {
	return id;
}

const string Device::getClass() const {
	return env["classname"].cast<string>();
}

Device::PIN_Interface::PIN_Interface(LuaRef& ref) :
		m_getPinLayout(ref["getPinLayout"]),
		m_getPin(ref["getPin"]), m_setPin(ref["setPin"]) {
	if(!implementsInterface(ref))
		cerr << "[Device] [PIN_Interface] WARN: Device " << ref << " not implementing interface" << endl;
}



bool Device::PIN_Interface::implementsInterface(const luabridge::LuaRef& ref) {
	// TODO: Better checks
	return ref["getPinLayout"].isFunction() &&
	       (ref["getPin"].isFunction() ||
	        ref["setPin"].isFunction());
}

Device::PIN_Interface::PinLayout Device::PIN_Interface::getPinLayout() {
	PinLayout ret;
	LuaResult r = m_getPinLayout();
	//cout << r.size() << " elements in pinlayout" << endl;
	ret.reserve(r.size());
	for(unsigned i = 0; i < r.size(); i++) {
		if(!r[i].isTable()){
			cerr << "pinlayout return value malformed:" << endl;
			cerr << i << "\t" << r[i] << endl;
			continue;
		}
		//cout << "\tElement " << i << ": " << r[i] << " with length " << r[i].length() << endl;
		if(r[i].length() < 2 || r[i].length() > 3) {
			cerr << "Pinlayout element " << i << " (" << r[i] << ") is malformed" << endl;
			continue;
		}
		PinDesc desc;
		auto number = r[i][1].cast<PinNumber>();
		desc.name = "undef";
		if(r[i].length() == 3)
			desc.name = r[i][3].tostring();

		const string direction_raw = r[i][2];
		if(direction_raw == "input") {
			desc.dir = PinDesc::Dir::input;
		} else if(direction_raw == "output") {
			desc.dir = PinDesc::Dir::output;
		} else if(direction_raw == "inout") {
			desc.dir = PinDesc::Dir::inout;
		} else {
			cerr << "Pinlayout element " << i << " (" << r[i] << "), direction " << direction_raw << " is malformed" << endl;
			continue;
		}
		//cout << "Mapping device's pin " << number << " (" << desc.name << ")" << endl;
		ret.emplace(number, desc);
	}
	return ret;
}


bool Device::PIN_Interface::getPin(PinNumber num) {
	const LuaResult r = m_getPin(num);
	if(!r || !r[0].isBool()) {
		cerr << "[lua] Device getPin returned malformed output" << endl;
		return false;
	}
	return r[0].cast<bool>();
}

void Device::PIN_Interface::setPin(PinNumber num, bool val) {
	m_setPin(num, val);
}

Device::SPI_Interface::SPI_Interface(LuaRef& ref) :
		m_send(ref["receiveSPI"]) {
	if(!implementsInterface(ref))
		cerr << ref << "not implementing SPI interface" << endl;
}


uint8_t Device::SPI_Interface::send(uint8_t byte) {
	LuaResult r = m_send(byte);
	if(r.size() != 1) {
		cerr << " send SPI function failed!" << endl;
		return 0;
	}
	if(!r[0].isNumber()) {
		cerr << " send SPI function returned invalid type " << r[0] << endl;
		return 0;
	}
	return r[0];
}

bool Device::SPI_Interface::implementsInterface(const LuaRef& ref) {
	return ref["receiveSPI"].isFunction();
}

Device::Config_Interface::Config_Interface(luabridge::LuaRef& ref) :
	m_getConf(ref["getConfig"]), m_setConf(ref["setConfig"]), m_env(ref){

};

bool Device::Config_Interface::implementsInterface(const luabridge::LuaRef& ref) {
	return !ref["getConfig"].isNil() && !ref["setConfig"].isNil();
}

Device::Config_Interface::Config Device::Config_Interface::getConfig(){
	Config ret;
	LuaResult r = m_getConf();
	//cout << r.size() << " elements in config" << endl;

	for(unsigned i = 0; i < r.size(); i++) {
		if(!r[i].isTable()){
			cerr << "config return value malformed:" << endl;
			cerr << i << "\t" << r[i] << endl;
			continue;
		}
		//cout << "\tElement " << i << ": " << r[i] << " with length " << r[i].length() << endl;
		if(r[i].length() != 2) {
			cerr << "Config element " << i << " (" << r[i] << ") is not a pair" << endl;
			continue;
		}

		LuaRef name = r[i][1];
		LuaRef value = r[i][2];

		if(!name.isString()) {
			cerr << "Config name " << name << " is not a string" << endl;
			continue;
		}

		switch(value.type()) {
		case LUA_TNUMBER:
			ret.emplace(
					name, ConfigElem{value.cast<typeof(ConfigElem::Value::integer)>()}
			);
			break;
		case LUA_TBOOLEAN:
			ret.emplace(
					name, ConfigElem{value.cast<bool>()}
			);
			break;
		default:
			cerr << "Config value of unknown type: " << value << endl;
		}
	}
	return ret;
}

bool Device::Config_Interface::setConfig(const Device::Config_Interface::Config conf) {
	LuaRef c = luabridge::newTable(m_env.state());
	for(auto& [name, elem] : conf) {
		c[name] = elem.value.integer;
	}
	return m_setConf(c).wasOk();
}

Device::Graphbuf_Interface::Graphbuf_Interface(luabridge::LuaRef& ref, lua_State* l) :
		m_getGraphBufferLayout(ref["getGraphBufferLayout"]),env(ref),L(l) {
	if(!implementsInterface(ref))
		cerr << "[Device] [Graphbuf_Interface] WARN: Device " << ref << " not implementing interface" << endl;

	registerPixelFormat(L);
}

Device::Graphbuf_Interface::Layout Device::Graphbuf_Interface::getLayout() {
	Layout ret = {0,0,"invalid"};
	LuaResult r = m_getGraphBufferLayout();
	if(r.size() != 1 || !r[0].isTable() || r[0].length() != 3) {
		cerr << "[Device] [Graphbuf_Interface] Layout malformed" << endl;
		return ret;
	}
	ret.width = r[0][0].cast<unsigned>();
	ret.height = r[0][1].cast<unsigned>();
	const auto& type = r[0][2];
	if(!type.isString() || type != string("rgba")) {
		cerr << "[Device] [Graphbuf_Interface] Layout type may only be 'rgba' at the moment." << endl;
		return ret;
	}
	ret.data_type = type.cast<string>();
	return ret;
}

void Device::Graphbuf_Interface::registerPixelFormat(lua_State* L) {
	auto testPixel = getGlobal(L, "graphbuf.Pixel");
	if(!testPixel(0,0,0,0)) {
		cout << "Testpixel could not be created, probably was not yet registered" << endl;
		luabridge::getGlobalNamespace(L)
			.beginNamespace("graphbuf")
			  .beginClass <Pixel> ("Pixel")
			    .addConstructor <void (*) (const uint8_t, const uint8_t, const uint8_t, const uint8_t)> ()
			    .addProperty ("r", &Pixel::r)
			    .addProperty ("g", &Pixel::g)
			    .addProperty ("b", &Pixel::b)
			    .addProperty ("a", &Pixel::a)
			  .endClass ()
			.endNamespace()
		;
	}
}

void Device::Graphbuf_Interface::registerSetBuf(const SetBuf_fn setBuf) {
	// TODO: Register global function and insert into local table

	/*
	const auto getGraphbuf_name = instance_id+"_getGraphbuf";
	const auto setGraphbuf_name = instance_id+"_setGraphbuf";

	luabridge::getGlobalNamespace(open_lua_states.back())
		  .addFunction(getGraphbuf_name.c_str(),
				[&mock_graphbuffers,instance_id](const unsigned offs){
					cout << "getGraphbuf for instance " << instance_id << endl;
					return mock_graphbuffers.at(instance_id)[offs];
				}
	);
	luabridge::getGlobalNamespace(open_lua_states.back())
		  .addFunction(setGraphbuf_name.c_str(),
				[&mock_graphbuffers,instance_id](const unsigned offs, const int val){
					cout << "setGraphbuf for instance " << instance_id << endl;
					mock_graphbuffers.at(instance_id)[offs] = val;
				}
	);
	const auto& device_table = devices.at(instance_id);
	const auto getGraphbuf_fun = getGlobal(device_table.L, getGraphbuf_name.c_str());
	const auto setGraphbuf_fun = getGlobal(device_table.L, setGraphbuf_name.c_str());
	device_table.env["getGraphbuf"] = getGraphbuf_fun;
	device_table.env["setGraphbuf"] = setGraphbuf_fun;
	 */
}