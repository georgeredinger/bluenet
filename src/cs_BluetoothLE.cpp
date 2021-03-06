/**
 * @author Christopher Mason
 * @author Anne van Rossum
 * License: TODO
 */

#include "cs_BluetoothLE.h"
#include "cs_nRF51822.h"
#include "util/cs_Utils.h"

#if(NORDIC_SDK_VERSION >= 6)
#include "nrf_soc.h"
#endif

#include "softdevice_handler.h"

#if CHAR_MESHING==1
extern "C" {
#include <protocol/cs_Mesh.h>
#include <protocol/rbc_mesh.h>
}
#endif

#ifndef SOFTDEVICE_SERIES
#error "The SOFTDEVICE_SERIES macro is required for compilation. Set it to 110 for example"
#endif

/**********************************************************************************************************************
 * Defines, local to this file
 *********************************************************************************************************************/

// use our own event handler (not the one in softdevice_handler.h)
#define OWN_EVENT_HANDLER

/**********************************************************************************************************************
 * Code start
 *********************************************************************************************************************/

using namespace BLEpp;

/**@brief Variable length data encapsulation in terms of length and pointer to data */

typedef struct {
	uint8_t * p_data; /**< Pointer to data. */
	uint16_t data_len; /**< Length of data. */
} data_t;

/// Characteristic /////////////////////////////////////////////////////////////////////////////////////////////////////

/* Set the default attributes of every characteristic
 *
 * There are two settings for the location of the memory of the buffer that is used to communicate with the SoftDevice.
 *
 * + BLE_GATTS_VLOC_STACK
 * + BLE_GATTS_VLOC_USER
 *
 * The former makes the SoftDevice allocate the memory (the quantity of which is defined by the user). The latter 
 * leaves it to the user. It is essential that with BLE_GATTS_VLOC_USER the calls to sd_ble_gatts_value_set() are
 * handed persistent pointers to a buffer. If a temporary value is used, this will screw up the data read by the user.
 * The same is true if this buffer is reused across characteristics. If it is meant as data for one characteristic
 * and is read through another, this will resolve to nonsense to the user.
 */
void set_attr_md_read_only(ble_gatts_attr_md_t& md, char vloc) {
	BLE_GAP_CONN_SEC_MODE_SET_OPEN(&md.read_perm);
	BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&md.write_perm);
	md.vloc = vloc;
	md.rd_auth = 0; // don't request read access every time a read is attempted.
	md.wr_auth = 0;  // ditto for write.
	md.vlen = 1;  // variable length.
}

void set_attr_md_read_write(ble_gatts_attr_md_t& md, char vloc) {
	BLE_GAP_CONN_SEC_MODE_SET_OPEN(&md.read_perm);
	BLE_GAP_CONN_SEC_MODE_SET_OPEN(&md.write_perm);
	md.vloc = vloc;
	md.rd_auth = 0; // don't request read access every time a read is attempted.
	md.wr_auth = 0;  // ditto for write.
	md.vlen = 1;  // variable length.
}

CharacteristicBase::CharacteristicBase() :
				_handles( { }), _service(0)
				/*nited(false), _notifies(false), _writable(false), */
				/*_unit(0), */ /*_updateIntervalMsecs(0),*/ /* _notifyingEnabled(false), _indicates(false) */
{
}

CharacteristicBase& CharacteristicBase::setName(const char * const name) {
	if (_status.inited) BLE_THROW(MSG_BLE_CHAR_INITIALIZED);
	_name = name;

	return *this;
}

/*
CharacteristicBase& CharacteristicBase::setUnit(uint16_t unit) {
	_unit = unit;
	return *this;
} */

//CharacteristicBase& CharacteristicBase::setUpdateIntervalMSecs(uint32_t msecs) {
//    if (_updateIntervalMsecs != msecs) {
//        _updateIntervalMsecs = msecs;
//        // TODO schedule task.
//        app_timer_create(&_timer, APP_TIMER_MODE_REPEATED, [](void* context){
//            CharacteristicBase* characteristic = (CharacteristicBase*)context;
//            characteristic->read();
//        });
//        app_timer_start(_timer, (uint32_t)(_updateIntervalMsecs / 1000. * (32768 / NRF51_RTC1_PRESCALER +1)), this);
//    }
//    return *this;
//}

void CharacteristicBase::init(Service* svc) {
	_service = svc;

	CharacteristicInit ci;

	// by default:

	ci.char_md.char_props.read = 1; // allow read
	ci.char_md.char_props.notify = 1; // allow notification
	ci.char_md.char_props.broadcast = 0; // allow broadcast
	ci.char_md.char_props.indicate = 0; // allow indication

	// initially readable but not writeable
	set_attr_md_read_write(ci.cccd_md, BLE_GATTS_VLOC_STACK);
	set_attr_md_read_only(ci.sccd_md, BLE_GATTS_VLOC_STACK);
	set_attr_md_read_only(ci.attr_md, BLE_GATTS_VLOC_USER);

	// these characteristic descriptors are optional, and I gather, not really used by anything.
	// we fill them in if the user specifies any of the data (eg name).
	ci.char_md.p_char_user_desc = NULL;
	ci.char_md.p_char_pf = NULL;
	ci.char_md.p_user_desc_md = NULL;
	ci.char_md.p_cccd_md = &ci.cccd_md; // the client characteristic metadata.
	//  _char_md.p_sccd_md         = &_sccd_md;

	_uuid.init();
	ble_uuid_t uid = _uuid;

	ci.attr_char_value.p_attr_md = &ci.attr_md;

	ci.attr_char_value.init_offs = 0;
	ci.attr_char_value.init_len = getValueLength();
	ci.attr_char_value.max_len = getValueMaxLength();
	ci.attr_char_value.p_value = getValuePtr();

	std::string name = std::string(_name);

	LOGd("%s init with buffer[%i] with %p", name.c_str(), getValueLength(), getValuePtr());

	ci.attr_char_value.p_uuid = &uid;

	setupWritePermissions(ci);

	if (!name.empty()) {
		ci.char_md.p_char_user_desc = (uint8_t*) name.c_str(); // todo utf8 conversion?
		ci.char_md.char_user_desc_size = name.length();
		ci.char_md.char_user_desc_max_size = name.length();
	}

	// This is the metadata (eg security settings) for the description of this characteristic.
	ci.char_md.p_user_desc_md = &ci.user_desc_metadata_md;

	set_attr_md_read_only(ci.user_desc_metadata_md, BLE_GATTS_VLOC_STACK);

	this->configurePresentationFormat(ci.presentation_format);
	ci.presentation_format.unit = 0; //_unit;
	ci.char_md.p_char_pf = &ci.presentation_format;

	volatile uint16_t svc_handle = svc->getHandle();

	uint32_t err_code = sd_ble_gatts_characteristic_add(svc_handle, &ci.char_md, &ci.attr_char_value, &_handles);
	if (err_code != NRF_SUCCESS) {
		LOGe(MSG_BLE_CHAR_CREATION_ERROR);
		APP_ERROR_CHECK(err_code);
	}

	//BLE_CALL(sd_ble_gatts_characteristic_add, (svc_handle, &ci.char_md, &ci.attr_char_value, &_handles));

	_status.inited = true;
}

void CharacteristicBase::setupWritePermissions(CharacteristicInit& ci) {
	// Dominik: why set it if the whole struct is being overwritten anyway futher down??!!
	//	ci.attr_md.write_perm.sm = _writable ? 1 : 0;
	//	ci.attr_md.write_perm.lv = _writable ? 1 : 0;
	ci.char_md.char_props.write = _status.writable ? 1 : 0;
	// Dominik: why is write wo response automatically enabled when writable? shouldn't it
	//  be handled independently?!
	ci.char_md.char_props.write_wo_resp = _status.writable ? 1 : 0;
	ci.char_md.char_props.notify = _status.notifies ? 1 : 0;
	// Dominik: agreed, indications seem to be almost the same as notifications, but they
	//  are not totally the same, so why set them together? they should be handled
	//  independently!!
	// ci.char_md.char_props.indicate = _notifies ? 1 : 0;
	ci.char_md.char_props.indicate = _status.indicates ? 1 : 0;
	ci.attr_md.write_perm = _writeperm;
	// Dominik: this overwrites the write permissions based on the security mode,
	// so even setting writable to false will always have the write permissions set
	// to true because security mode of 0 / 0 is not defined by the Bluetooth Core
	// specification anyway. security mode is always at least 1 / 1. Doesnt make sense !!!
	//	ci.char_md.char_props.write = ci.cccd_md.write_perm.lv > 0 ? 1 :0;
	//	ci.char_md.char_props.write_wo_resp = ci.cccd_md.write_perm.lv > 0 ? 1 :0;
}

uint32_t CharacteristicBase::notify() {

	uint16_t valueLength = getValueLength();
	uint8_t* valueAddress = getValuePtr();

#if ($SOFTDEVICE_SERIES == 130) && ($SOFTDEVICE_MAJOR == 0) && ($SOFTDEVICE_MINOR == 9)
	ble_gatts_value_t p_value;
	p_value.len = valueLength;
	p_value.offset = 0;
	p_value.p_value = valueAddress;
	// leads to error... 0007, invalid param, don't know why!
	BLE_CALL(sd_ble_gatts_value_set,	(
			_service->getStack()->getConnectionHandle(),
			_handles.value_handle, &p_value));
#else
	BLE_CALL(sd_ble_gatts_value_set,
			(_handles.value_handle, 0, &valueLength, valueAddress));
#endif

	// stop here if we are not in notifying state
	if ((!_status.notifies) || (!_service->getStack()->connected()) || !_status.notifyingEnabled) {
		return NRF_SUCCESS;
	}

	ble_gatts_hvx_params_t hvx_params;
	uint16_t len = valueLength;

	hvx_params.handle = _handles.value_handle;
	hvx_params.type = BLE_GATT_HVX_NOTIFICATION;
	hvx_params.offset = 0;
	hvx_params.p_len = &len;
	hvx_params.p_data = valueAddress;

	//	BLE_CALL(sd_ble_gatts_hvx, (_service->getStack()->getConnectionHandle(), &hvx_params));
	uint32_t err_code = sd_ble_gatts_hvx(_service->getStack()->getConnectionHandle(), &hvx_params);
	if (err_code != NRF_SUCCESS) {

		if (err_code == BLE_ERROR_NO_TX_BUFFERS) {
			// Dominik: this happens if several characteristics want to send a notification,
			//   but the system only has a limited number of tx buffers available. so queueing up
			//   notifications faster than being able to send them out from the stack results
			//   in this error.
			onNotifyTxError();
		} else if (err_code == NRF_ERROR_INVALID_STATE) {
			// Dominik: if a characteristic is updating it's value "too fast" and notification is enabled
			//   it can happen that it tries to update it's value although notification was disabled in
			//   in the meantime, in which case an invalid state error is returned. but this case we can
			//   ignore

			// this is not a serious error, but better to at least write it to the log
			LOGd("ERR_CODE: %d (0x%X)", err_code, err_code);
		} else if (err_code == BLE_ERROR_GATTS_SYS_ATTR_MISSING) {
			// Anne: do not complain for now... (meshing)
		} else {
			APP_ERROR_CHECK(err_code);
		} 
	}

	return err_code;
}

void CharacteristicBase::onNotifyTxError() {
	// in most cases we can just ignore the error, because the characteristic is updated frequently
	// so the next update will probably be done as soon as the tx buffers are ready anyway, but in
	// case there are characteristics that only get updated really infrequently, the notification
	// should be buffered and sent again once the tx buffers are ready
	//	LOGd("[%s] no tx buffers, notification skipped!", _name.c_str());
}

void CharacteristicBase::onTxComplete(ble_common_evt_t * p_ble_evt) {
	// if a characteristic buffers notification when it runs out of tx buffers it can use
	// this callback to resend the buffered notification
}

/// Service ////////////////////////////////////////////////////////////////////////////////////////////////////////////

const char* Service::defaultServiceName = "unnamed";

void Service::start(Nrf51822BluetoothStack* stack) {

	_stack = stack;

	uint32_t err_code;

	_uuid.init();
	const ble_uuid_t uuid = _uuid;

	_service_handle = BLE_CONN_HANDLE_INVALID;
	err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &uuid,
			(uint16_t*) &_service_handle);
	APP_ERROR_CHECK(err_code);

	for (CharacteristicBase* characteristic : getCharacteristics()) {
		characteristic->init(this);
	}

	_started = true;

}

/**
 * Seperate function that actually adds the characteristics. This allows to introduce dependencies between construction
 * of the different services and the characteristics on those services.
 */
//void GenericService::addSpecificCharacteristics() {
//	for ( CharacteristicStatusT &status : characStatus) {
//		if (status.enabled) {
//			LOGi("Create characteristic %s (%i)", status.name.c_str(), status.UUID);
//			(this->*status.func)();
//		} else {
//			LOGi("Disabled characteristic %s (%i)", status.name.c_str(), status.UUID);
//		}
//	}
//}

/**
 * A service can receive a BLE event. Currently we pass the connection events through as well as the write event.
 * The latter is wired through on_write() which we pass the evt.gatts_evt.params.write part of the event.
 */
void Service::on_ble_event(ble_evt_t * p_ble_evt) {
	switch (p_ble_evt->header.evt_id) {
	case BLE_GAP_EVT_CONNECTED:
		on_connect(p_ble_evt->evt.gap_evt.conn_handle, p_ble_evt->evt.gap_evt.params.connected);
		break;

	case BLE_GAP_EVT_DISCONNECTED:
		on_disconnect(p_ble_evt->evt.gap_evt.conn_handle, p_ble_evt->evt.gap_evt.params.disconnected);
		break;

	case BLE_GATTS_EVT_WRITE:
		on_write(p_ble_evt->evt.gatts_evt.params.write);
		break;

	default:
		break;
	}
}

/* On connect event for an individual service
 *
 * An individual service normally doesn't respond to a connect event. However, it can be used to for example allocate
 * memory only when a user is connected.
 */
void Service::on_connect(uint16_t conn_handle, ble_gap_evt_connected_t& gap_evt) {
	// nothing here yet.
}

/* On disconnect event for an individual service
 *
 * Just as on_connect this method can be used to for example deallocate structures that only need to exist when a user
 * is connected.
 */
void Service::on_disconnect(uint16_t conn_handle, ble_gap_evt_disconnected_t& gap_evt) {
	// nothing here yet.
}

/* Write incoming value to data structures on the device.
 *
 * On an incoming write event we go through the characteristics that belong to this service and compare a handle
 * to see which one we have to write. Subsequently, characteristic->written will be called with length, offset, and
 * the data itself. Depending on the characteristic this can be retrieved as a string or any kind of data type.
 * There is currently no exception handling when the characteristic cannot be found.
 *
 * Note that the write can also be not the value, but the flag to enable or disable notification. In this case 
 * write_evt.handle is compared instead of write_evt.context.value_handle. Of course, the corresponding handle in
 * the characteristic object is also different.
 */
void Service::on_write(ble_gatts_evt_write_t& write_evt) {
	bool found = false;

	for (CharacteristicBase* characteristic : getCharacteristics()) {

		if (characteristic->getCccdHandle() == write_evt.handle && write_evt.len == 2) {
			// received write to enable/disable notification
			characteristic->setNotifyingEnabled(ble_srv_is_notification_enabled(write_evt.data));
			found = true;

		} else if (characteristic->getValueHandle() == write_evt.context.value_handle) {
			// TODO: make a map.
			found = true;

			if (write_evt.op == BLE_GATTS_OP_WRITE_REQ
					|| write_evt.op == BLE_GATTS_OP_WRITE_CMD
					|| write_evt.op == BLE_GATTS_OP_SIGN_WRITE_CMD) {
				characteristic->written(write_evt.len, write_evt.offset,
						write_evt.data);
			} else {
				found = false;
			}
		}
	}

	if (!found) {
		// tell someone?
		LOGe(MSG_BLE_CHAR_CANNOT_FIND);
	}
}

// inform all characteristics that transmission was completed in case they have notifications pending
void Service::onTxComplete(ble_common_evt_t * p_ble_evt) {
	for (CharacteristicBase* characteristic : getCharacteristics()) {
		characteristic->onTxComplete(p_ble_evt);
	}
}

/// Nrf51822BluetoothStack /////////////////////////////////////////////////////////////////////////////////////////////

Nrf51822BluetoothStack::Nrf51822BluetoothStack() :
				_appearance(defaultAppearance), _clock_source(defaultClockSource), _mtu_size(defaultMtu),
				_tx_power_level(defaultTxPowerLevel), _sec_mode({ }),
				_interval(defaultAdvertisingInterval_0_625_ms), _timeout(defaultAdvertisingTimeout_seconds),
			  	_gap_conn_params( { }),
				_inited(false), _started(false), _advertising(false), _scanning(false),
				_conn_handle(BLE_CONN_HANDLE_INVALID),
				_radio_notify(0)
{
	_evt_buffer_size = sizeof(ble_evt_t) + (_mtu_size) * sizeof(uint32_t);
	_evt_buffer = (uint8_t*) malloc(_evt_buffer_size);

	//if (!is_word_aligned(_evt_buffer)) throw ble_exception("Event buffer not aligned.");

	// setup default values.

	BLE_GAP_CONN_SEC_MODE_SET_OPEN(&_sec_mode);

	_gap_conn_params.min_conn_interval = defaultMinConnectionInterval_1_25_ms;
	_gap_conn_params.max_conn_interval = defaultMaxConnectionInterval_1_25_ms;
	_gap_conn_params.slave_latency = defaultSlaveLatencyCount;
	_gap_conn_params.conn_sup_timeout = defaultConnectionSupervisionTimeout_10_ms;
}

Nrf51822BluetoothStack::~Nrf51822BluetoothStack() {
	shutdown();

	if (_evt_buffer)
		free(_evt_buffer);
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::init() {

	if (_inited)
		return *this;

	// Initialise SoftDevice
	uint8_t enabled;
	BLE_CALL(sd_softdevice_is_enabled, (&enabled));
	if (enabled) {
		LOGw(MSG_BLE_SOFTDEVICE_RUNNING);
		BLE_CALL(sd_softdevice_disable, ());
	}

	LOGd(MSG_BLE_SOFTDEVICE_INIT);
	// Initialize the SoftDevice handler module.
	// this would call with different clock!
	SOFTDEVICE_HANDLER_INIT(_clock_source, false);
	
	// enable the BLE stack
	LOGd(MSG_BLE_SOFTDEVICE_ENABLE);
//#if(SOFTDEVICE_SERIES == 110) 

#if (SOFTDEVICE_SERIES != 130) && (SOFTDEVICE_MINOR != 5) 
#if(NORDIC_SDK_VERSION >= 6)
	// do not define the service_changed characteristic, of course allow future changes
#define IS_SRVC_CHANGED_CHARACT_PRESENT  1
	ble_enable_params_t ble_enable_params;
	memset(&ble_enable_params, 0, sizeof(ble_enable_params));
	ble_enable_params.gatts_enable_params.service_changed = IS_SRVC_CHANGED_CHARACT_PRESENT;
	BLE_CALL(sd_ble_enable, (&ble_enable_params) );
#endif
#endif
//#endif

	// according to the migration guide the address needs to be set directly after the sd_ble_enable call
	// due to "an issue present in the s110_nrf51822_7.0.0 release"
#if(SOFTDEVICE_SERIES == 110) 
#if(NORDIC_SDK_VERSION >= 7)
	LOGd(MSG_BLE_SOFTDEVICE_ENABLE_GAP);
	BLE_CALL(sd_ble_gap_enable, () );
	ble_gap_addr_t addr;
	BLE_CALL(sd_ble_gap_address_get, (&addr) );
	BLE_CALL(sd_ble_gap_address_set, (BLE_GAP_ADDR_CYCLE_MODE_NONE, &addr) );
#endif
#endif
	// version is not saved or shown yet
	ble_version_t version( { });
	version.company_id = 12;
	BLE_CALL(sd_ble_version_get, (&version));

	sd_nvic_EnableIRQ(SWI2_IRQn);

	if (!_device_name.empty()) {
		BLE_CALL(sd_ble_gap_device_name_set,
				(&_sec_mode, (uint8_t*) _device_name.c_str(), _device_name.length()));
	} else {
		std::string name = "not set...";
		BLE_CALL(sd_ble_gap_device_name_set,
				(&_sec_mode, (uint8_t*) name.c_str(), name.length()));
	} 
	BLE_CALL(sd_ble_gap_appearance_set, (_appearance));

	setConnParams();

	setTxPowerLevel();

	BLE_CALL(softdevice_sys_evt_handler_set, (sys_evt_dispatch));

	_inited = true;

	return *this;
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::updateDeviceName(const std::string& deviceName) {
	_device_name = deviceName;
	if (!_device_name.empty()) {
		BLE_CALL(sd_ble_gap_device_name_set,
				(&_sec_mode, (uint8_t*) _device_name.c_str(), _device_name.length()));
	} else {
		std::string name = "not set...";
		BLE_CALL(sd_ble_gap_device_name_set,
				(&_sec_mode, (uint8_t*) name.c_str(), name.length()));
	}
	return *this;
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::start() {
	if (_started)
		return *this;

	for (Service* svc : _services) {
		svc->start(this);
	}

	_started = true;

	return *this;
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::shutdown() {
	stopAdvertising();

	for (Service* svc : _services) {
		svc->stop();
	}

	_inited = false;

	return *this;
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::addService(Service* svc) {
	//APP_ERROR_CHECK_BOOL(_services.size() < MAX_SERVICE_COUNT);

	_services.push_back(svc);

	return *this;
}

void Nrf51822BluetoothStack::setTxPowerLevel() {
	BLE_CALL(sd_ble_gap_tx_power_set, (_tx_power_level));
}

void Nrf51822BluetoothStack::setConnParams() {
	BLE_CALL(sd_ble_gap_ppcp_set, (&_gap_conn_params));
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::setTxPowerLevel(int8_t powerLevel) {
	if (_tx_power_level != powerLevel) {
		_tx_power_level = powerLevel;
		if (_inited)
			setTxPowerLevel();
	}
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::startIBeacon(IBeacon beacon) {
	if (_advertising)
		return *this;

	LOGi("startIBeacon ...");

	init(); // we should already be.

	start();

	uint32_t err_code __attribute__((unused));
	ble_advdata_t advdata;
	uint8_t flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
	//	uint8_t flags = BLE_GAP_ADV_FLAG_LE_GENERAL_DISC_MODE | BLE_GAP_ADV_FLAG_LE_BR_EDR_CONTROLLER
	//			| BLE_GAP_ADV_FLAG_LE_BR_EDR_HOST;

	ble_gap_adv_params_t adv_params;

	adv_params.type = BLE_GAP_ADV_TYPE_ADV_IND;
	adv_params.p_peer_addr = NULL;                   // Undirected advertisement
	adv_params.fp = BLE_GAP_ADV_FP_ANY;
	adv_params.interval = _interval;
	adv_params.timeout = _timeout;

	ble_advdata_manuf_data_t manufac;
	manufac.company_identifier = 0x004C; // Apple Company ID, if it is not set to apple it's not recognized as an iBeacon

	uint8_t adv_manuf_data[beacon.size()];
	memset(adv_manuf_data, 0, sizeof(adv_manuf_data));
	beacon.toArray(adv_manuf_data);

	manufac.data.p_data = adv_manuf_data;
	manufac.data.size = beacon.size();

	// Build and set advertising data
	memset(&advdata, 0, sizeof(advdata));

	advdata.flags.size = sizeof(flags);
	advdata.flags.p_data = &flags;
	advdata.p_manuf_specific_data = &manufac;

	ble_advdata_t scan_resp;
	memset(&scan_resp, 0, sizeof(scan_resp));

	//	uint8_t uidCount = _services.size();
	//	ble_uuid_t adv_uuids[uidCount];
	//
	//	uint8_t cnt = 0;
	//	for(Service* svc : _services ) {
	//		adv_uuids[cnt++] = svc->getUUID();
	//	}
	//
	//	if (cnt == 0) {
	//		LOGw("No custom services!");
	//	}

	scan_resp.name_type = BLE_ADVDATA_FULL_NAME;
	//	scan_resp.uuids_more_available.uuid_cnt = 1;
	//	scan_resp.uuids_more_available.p_uuids  = adv_uuids;

	BLE_CALL(ble_advdata_set, (&advdata, &scan_resp));

	BLE_CALL(sd_ble_gap_adv_start, (&adv_params));

	_advertising = true;

	LOGi("... OK");

	return *this;
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::startAdvertising() {
	if (_advertising)
		return *this;

	LOGi(MSG_BLE_ADVERTISING_START);

	init(); // we should already be.

	start();

	uint32_t err_code __attribute__((unused));
	ble_advdata_t advdata;
	uint8_t flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;

	uint8_t uidCount = _services.size();
	LOGi("Number of services: %u", uidCount);

	ble_uuid_t adv_uuids[uidCount];

	uint8_t cnt = 0;
	for (Service* svc : _services) {
		adv_uuids[cnt++] = svc->getUUID();
	}

	if (cnt == 0) {
		LOGw(MSG_BLE_NO_CUSTOM_SERVICES);
	}

	ble_gap_adv_params_t adv_params;

	adv_params.type = BLE_GAP_ADV_TYPE_ADV_IND;
	adv_params.p_peer_addr = NULL;                   // Undirected advertisement
	adv_params.fp = BLE_GAP_ADV_FP_ANY;
	adv_params.interval = _interval;
	adv_params.timeout = _timeout;

	// Build and set advertising data
	memset(&advdata, 0, sizeof(advdata));

	ble_advdata_manuf_data_t manufac;
	// TODO: made up ID, has to be replaced by official ID
	manufac.company_identifier = 0x1111; // DoBots Company ID
	manufac.data.size = 0;

	//	advdata.name_type               = BLE_ADVDATA_NO_NAME;

	/*
	 * 31 bytes total payload
	 *
	 * 1 byte per element. in this case, 3 elements -> 3 bytes
	 *
	 * => 28 bytes available payload
	 *
	 * 2 bytes for flags (1 byte for type, 1 byte for data)
	 * 2 bytes for tx power level (1 byte for type, 1 byte for data)
	 * 17 bytes for UUID (1 byte for type, 16 byte for data)
	 *
	 * -> 7 bytes left
	 *
	 * Note: by adding an element, one byte will be lost because of the
	 *   addition, so there are actually only 6 bytes left after adding
	 *   an element, and 1 byte of that is used for the type, so 5 bytes
	 *   are left for the data
	 */

	// Anne: setting NO_NAME breaks the Android Nordic nRF Master Console app.
	advdata.p_tx_power_level = &_tx_power_level;
	advdata.flags.size = sizeof(flags);
	advdata.flags.p_data = &flags;
	advdata.p_manuf_specific_data = &manufac;
	// TODO -oDE: It doesn't really make sense to have this function in the library
	//  because it really depends on the application on how many and what kind
	//  of services are available and what should be advertised
	//#ifdef YOU_WANT_TO_USE_SPACE
	if (uidCount > 1) {
		advdata.uuids_more_available.uuid_cnt = 1;
		advdata.uuids_more_available.p_uuids = adv_uuids;
	} else {
		advdata.uuids_complete.uuid_cnt = 1;
		advdata.uuids_complete.p_uuids = adv_uuids;
	}
	//#endif

	// Because of the limited amount of space in the advertisement data, additional
	// data can be supplied in the scan response package. Same space restrictions apply
	// here:
	/*
	 * 31 bytes total payload
	 *
	 * 1 byte per element. in this case, 1 element -> 1 bytes
	 *
	 * 30 bytes of available payload
	 *
	 * 1 byte for name type
	 * -> 29 bytes left for name
	 */
	ble_advdata_t scan_resp;
	memset(&scan_resp, 0, sizeof(scan_resp));
	scan_resp.name_type = BLE_ADVDATA_FULL_NAME;

	err_code = ble_advdata_set(&advdata, &scan_resp);
	if (err_code == NRF_ERROR_DATA_SIZE) {
		log(FATAL, MSG_BLE_ADVERTISEMENT_TOO_BIG);
	} 
	APP_ERROR_CHECK(err_code);

	// segfault when advertisement cannot start, do we want that!?
	//BLE_CALL(sd_ble_gap_adv_start, (&adv_params));
	err_code = sd_ble_gap_adv_start(&adv_params);
	if (err_code == NRF_ERROR_INVALID_PARAM) {
		log(FATAL, MSG_BLE_ADVERTISEMENT_CONFIG_INVALID);
	}
	APP_ERROR_CHECK(err_code);

	_advertising = true;

	LOGi(MSG_BLE_ADVERTISING_STARTED);

	return *this;
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::stopAdvertising() {
	if (!_advertising)
		return *this;

	BLE_CALL(sd_ble_gap_adv_stop, ());

	_advertising = false;

	return *this;
}

bool Nrf51822BluetoothStack::isAdvertising() {
	return _advertising;
}

// See https://devzone.nordicsemi.com/question/21164/s130-unstable-advertising-reports-during-scan-updated/
#define SCAN_INTERVAL                    0x00A0         /**< Determines scan interval in units of 0.625 millisecond. */
//#define SCAN_WINDOW                      0x0050         /**< Determines scan window in units of 0.625 millisecond. */
#define SCAN_WINDOW                      0x009E         /**< Determines scan window in units of 0.625 millisecond. */

Nrf51822BluetoothStack& Nrf51822BluetoothStack::startScanning() {
#if(SOFTDEVICE_SERIES != 110)
	if (_scanning)
		return *this;
	LOGi("startScanning");
	ble_gap_scan_params_t p_scan_params;
	// No devices in whitelist, hence non selective performed.
	p_scan_params.active = 0;            // Active scanning set.
	p_scan_params.selective = 0;            // Selective scanning not set.
	p_scan_params.interval = SCAN_INTERVAL;            // Scan interval.
	p_scan_params.window = SCAN_WINDOW;  // Scan window.
	p_scan_params.p_whitelist = NULL;         // No whitelist provided.
	p_scan_params.timeout = 0x0000;       // No timeout.

	// todo: which fields to set here?
	BLE_CALL(sd_ble_gap_scan_start, (&p_scan_params));
	_scanning = true;
#endif
	return *this;
}


Nrf51822BluetoothStack& Nrf51822BluetoothStack::stopScanning() {
#if(SOFTDEVICE_SERIES != 110)
	if (!_scanning)
		return *this;
	LOGi("stopScanning");
	BLE_CALL(sd_ble_gap_scan_stop, ());
	_scanning = false;
#endif
	return *this;
}

bool Nrf51822BluetoothStack::isScanning() {
#if(SOFTDEVICE_SERIES != 110)
	return _scanning;
#endif
	return false;
}

Nrf51822BluetoothStack& Nrf51822BluetoothStack::onRadioNotificationInterrupt(
		uint32_t distanceUs, callback_radio_t callback) {
	_callback_radio = callback;

	nrf_radio_notification_distance_t distance = NRF_RADIO_NOTIFICATION_DISTANCE_NONE;

	if (distanceUs >= 5500) {
		distance = NRF_RADIO_NOTIFICATION_DISTANCE_5500US;
	} else if (distanceUs >= 4560) {
		distance = NRF_RADIO_NOTIFICATION_DISTANCE_4560US;
	} else if (distanceUs >= 3620) {
		distance = NRF_RADIO_NOTIFICATION_DISTANCE_3620US;
	} else if (distanceUs >= 2680) {
		distance = NRF_RADIO_NOTIFICATION_DISTANCE_2680US;
	} else if (distanceUs >= 1740) {
		distance = NRF_RADIO_NOTIFICATION_DISTANCE_1740US;
	} else if (distanceUs >= 800) {
		distance = NRF_RADIO_NOTIFICATION_DISTANCE_800US;
	}

	uint32_t result = sd_nvic_SetPriority(SWI1_IRQn, NRF_APP_PRIORITY_LOW);
	BLE_THROW_IF(result, "Could not set radio notification IRQ priority.");

	result = sd_nvic_EnableIRQ(SWI1_IRQn);
	BLE_THROW_IF(result, "Could not enable radio notification IRQ.");

	result = sd_radio_notification_cfg_set(
			distance == NRF_RADIO_NOTIFICATION_DISTANCE_NONE ?
					NRF_RADIO_NOTIFICATION_TYPE_NONE :
					NRF_RADIO_NOTIFICATION_TYPE_INT_ON_BOTH, distance);
	BLE_THROW_IF(result, "Could not configure radio notification.");

	return *this;
}

//extern "C" {
//	void SWI1_IRQHandler() { // radio notification IRQ handler
//		uint8_t radio_notify;
//		Nrf51822BluetoothStack::_stack->_radio_notify = radio_notify = ( Nrf51822BluetoothStack::_stack->_radio_notify + 1) % 2;
//		Nrf51822BluetoothStack::_stack->_callback_radio(radio_notify == 1);
//	}
//
//	void SWI2_IRQHandler(void) { // sd event IRQ handler
//		// do nothing.
//	}
//}

void Nrf51822BluetoothStack::tick() {

#if TICK_CONTINUOUSLY==0
#if(NORDIC_SDK_VERSION > 5)
	BLE_CALL(sd_app_evt_wait, ());
#else
	BLE_CALL(sd_app_event_wait, () );
#endif
#endif
	while (1) {

		//        uint8_t nested;
		//        sd_nvic_critical_region_enter(&nested);
		//        uint8_t radio_notify = _radio_notify = (radio_notify + 1) % 4;
		//        sd_nvic_critical_region_exit(nested);
		//        if ((radio_notify % 2 == 0) && _callback_radio) {
		//            _callback_radio(radio_notify == 0);
		//        }

		uint16_t evt_len = _evt_buffer_size;
		uint32_t err_code = sd_ble_evt_get(_evt_buffer, &evt_len);
		if (err_code == NRF_ERROR_NOT_FOUND) {
			break;
		}
		BLE_THROW_IF(err_code, "Error retrieving bluetooth event from stack.");

		on_ble_evt((ble_evt_t *) _evt_buffer);

	}
}

void Nrf51822BluetoothStack::on_ble_evt(ble_evt_t * p_ble_evt) {
	//	if (p_ble_evt->header.evt_id != BLE_GAP_EVT_RSSI_CHANGED) {
	//		LOGd("on_ble_event: %X", p_ble_evt->header.evt_id);
	//	}
#if CHAR_MESHING==1
	APP_ERROR_CHECK(rbc_mesh_ble_evt_handler(p_ble_evt));
#endif

	switch (p_ble_evt->header.evt_id) {
	case BLE_GAP_EVT_CONNECTED:
		on_connected(p_ble_evt);
		break;

	case BLE_GAP_EVT_DISCONNECTED:
		on_disconnected(p_ble_evt);
		break;

	case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST:
	case BLE_GATTS_EVT_TIMEOUT:
	case BLE_GAP_EVT_RSSI_CHANGED:
		for (Service* svc : _services) {
			svc->on_ble_event(p_ble_evt);
		}
		break;

	case BLE_GATTS_EVT_WRITE:
		for (Service* svc : _services) {
			if (svc->getHandle() == p_ble_evt->evt.gatts_evt.params.write.context.srvc_handle) {
				svc->on_ble_event(p_ble_evt);
			}
		}
		break;

	case BLE_GATTS_EVT_HVC:
		for (Service* svc : _services) {
			if (svc->getHandle() == p_ble_evt->evt.gatts_evt.params.write.context.srvc_handle) {
				svc->on_ble_event(p_ble_evt);
			}
		}
		break;

	case BLE_GATTS_EVT_SYS_ATTR_MISSING:
		BLE_CALL(sd_ble_gatts_sys_attr_set, (_conn_handle, NULL, 0));
		break;

	case BLE_GAP_EVT_SEC_PARAMS_REQUEST:

		ble_gap_sec_params_t sec_params;

#if(SOFTDEVICE_SERIES == 110) 
		sec_params.timeout = 30; // seconds
#endif
		sec_params.bond = 1;  // perform bonding.
		sec_params.mitm = 0;  // man in the middle protection not required.
		sec_params.io_caps = BLE_GAP_IO_CAPS_NONE;  // no display capabilities.
		sec_params.oob = 0;  // out of band not available.
		sec_params.min_key_size = 7;  // min key size
		sec_params.max_key_size = 16; // max key size.

#if(SOFTDEVICE_SERIES != 110) 
		// https://devzone.nordicsemi.com/documentation/nrf51/6.0.0/s120/html/a00527.html#ga7b23027c97b3df21f6cbc23170e55663

		// do not store the keys for now...
		//ble_gap_sec_keyset_t sec_keyset;
		//BLE_CALL(sd_ble_gap_sec_params_reply, (p_ble_evt->evt.gap_evt.conn_handle,
		//			BLE_GAP_SEC_STATUS_SUCCESS,
		//			&sec_params, &sec_keyset) );
		BLE_CALL(sd_ble_gap_sec_params_reply,
				(p_ble_evt->evt.gap_evt.conn_handle, BLE_GAP_SEC_STATUS_SUCCESS, &sec_params, NULL));
#else 
		BLE_CALL(sd_ble_gap_sec_params_reply, (p_ble_evt->evt.gap_evt.conn_handle,
				BLE_GAP_SEC_STATUS_SUCCESS,
				&sec_params) );
#endif
		break;
#if(SOFTDEVICE_SERIES != 110) 
	case BLE_GAP_EVT_ADV_REPORT:
		for (Service* svc : _services) {
			svc->on_ble_event(p_ble_evt);
		}
		break;
#endif
	case BLE_GAP_EVT_TIMEOUT:
		break;

	case BLE_EVT_TX_COMPLETE:
		onTxComplete(p_ble_evt);
		break;

	default:
		break;

	}
}

void Nrf51822BluetoothStack::on_connected(ble_evt_t * p_ble_evt) {
	//ble_gap_evt_connected_t connected_evt = p_ble_evt->evt.gap_evt.params.connected;
	_advertising = false; // it seems like maybe we automatically stop advertising when we're connected.

	BLE_CALL(sd_ble_gap_conn_param_update, (p_ble_evt->evt.gap_evt.conn_handle, &_gap_conn_params));

	if (_callback_connected) {
		_callback_connected(p_ble_evt->evt.gap_evt.conn_handle);
	}
	_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
	for (Service* svc : _services) {
		svc->on_ble_event(p_ble_evt);
	}
}

void Nrf51822BluetoothStack::on_disconnected(ble_evt_t * p_ble_evt) {
	//ble_gap_evt_disconnected_t disconnected_evt = p_ble_evt->evt.gap_evt.params.disconnected;
	if (_callback_connected) {
		_callback_disconnected(p_ble_evt->evt.gap_evt.conn_handle);
	}
	_conn_handle = BLE_CONN_HANDLE_INVALID;
	for (Service* svc : _services) {
		svc->on_ble_event(p_ble_evt);
	}
}

void Nrf51822BluetoothStack::onTxComplete(ble_evt_t * p_ble_evt) {
	for (Service* svc: _services) {
		svc->onTxComplete(&p_ble_evt->evt.common_evt);
	}
}

