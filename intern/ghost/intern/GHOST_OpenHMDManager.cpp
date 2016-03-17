/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "GHOST_OpenHMDManager.h"
#include "GHOST_EventOpenHMD.h"
#include "GHOST_WindowManager.h"

#include "include/openhmd.h"

GHOST_OpenHMDManager::GHOST_OpenHMDManager(GHOST_System& sys)
	: m_system(sys),
	  m_available(false),
	  m_context(NULL),
	  m_device(NULL),
	  m_deviceIndex(-1)
{
	m_context = ohmd_ctx_create();
	if (m_context != NULL) {

		int num_devices = ohmd_ctx_probe(m_context);
		if (num_devices > 0) {
			m_available = true;

			//can't fail?
			m_deviceIndex = 0;
			m_device = ohmd_list_open_device(m_context, m_deviceIndex);
		}
		else {
			printf("No available devices in OpenHMD Context\n");

			ohmd_ctx_destroy(m_context);
			m_context = NULL;
		}
	}
	else {
		printf("Failed to create OpenHMD Context\n");
	}
}

GHOST_OpenHMDManager::~GHOST_OpenHMDManager()
{
	if (m_available) {
		ohmd_ctx_destroy(m_context);
		m_context = NULL;
		m_device = NULL;
		m_available = false;
	}
}

bool GHOST_OpenHMDManager::processEvents()
{
	if (m_available) {
		GHOST_IWindow *window = m_system.getWindowManager()->getActiveWindow();

		if (!window)
			return false;


		GHOST_TUns64 now = m_system.getMilliSeconds();
		GHOST_EventOpenHMD *event = new GHOST_EventOpenHMD(now, window);
		GHOST_TEventOpenHMDData* data = (GHOST_TEventOpenHMDData*) event->getData();

		ohmd_ctx_update(m_context);
		if (!getRotationQuat(data->orientation))
			return false;

		m_system.pushEvent(event);
		return true;
	}
	else {
		return false;
	}
}

bool GHOST_OpenHMDManager::available() const
{
	return m_available;
}

bool GHOST_OpenHMDManager::setDevice(const char *requested_vendor_name, const char *requested_device_name)
{
	if (!m_available) {
		return false;
	}

	bool success = false;
	int num_devices = ohmd_ctx_probe(m_context);
	for (int i = 0; i < num_devices; ++i) {
		const char* device_name = ohmd_list_gets(m_context, i, OHMD_PRODUCT);
		const char* vendor_name = ohmd_list_gets(m_context, i, OHMD_VENDOR);

		if (strcmp(device_name, requested_device_name) == 0 && strcmp(vendor_name, requested_vendor_name) == 0) {
			success = setDevice(i);
			break;
		}
	}

	return success;
}

bool GHOST_OpenHMDManager::setDevice(int index)
{
	if (!m_available) {
		return false;
	}

	//out of bounds
	if (index >= ohmd_ctx_probe(m_context)) {
		return false;
	}

	m_deviceIndex = index;
	m_device = ohmd_list_open_device(m_context, index);
	return true;
}

int GHOST_OpenHMDManager::getNumDevices() const
{
	if (!m_available)
		return -1;

	return ohmd_ctx_probe(m_context);
}

const char *GHOST_OpenHMDManager::getError() const
{
	if (!m_available) {
		return NULL;
	}

	return ohmd_ctx_get_error(m_context);
}

const char *GHOST_OpenHMDManager::getDeviceName() const
{
	if (!m_available)
		return NULL;

	return ohmd_list_gets(m_context, m_deviceIndex, OHMD_PRODUCT);
}

const char *GHOST_OpenHMDManager::getVendorName() const
{
	if (!m_available)
		return NULL;

	return ohmd_list_gets(m_context, m_deviceIndex, OHMD_VENDOR);
}

const char *GHOST_OpenHMDManager::getPath() const
{
	if (!m_available)
		return NULL;

	return ohmd_list_gets(m_context, m_deviceIndex, OHMD_PATH);
}

bool GHOST_OpenHMDManager::getRotationQuat(float orientation[4]) const
{
	if (!m_available) {
		return false;
	}

	float tmp[4];
	if (ohmd_device_getf(m_device, OHMD_ROTATION_QUAT, tmp) < 0)
		return false;

	orientation[0] = tmp[3];
	orientation[1] = tmp[0];
	orientation[2] = tmp[1];
	orientation[3] = tmp[2];

	return true;
}

void GHOST_OpenHMDManager::getLeftEyeGLModelviewMatrix(float mat[16]) const
{
	if (!m_available) {
		return;
	}

	ohmd_device_getf(m_device, OHMD_LEFT_EYE_GL_MODELVIEW_MATRIX, mat);
}

void GHOST_OpenHMDManager::getRightEyeGLModelviewMatrix(float mat[16]) const
{
	if (!m_available) {
		return;
	}

	ohmd_device_getf(m_device, OHMD_RIGHT_EYE_GL_MODELVIEW_MATRIX, mat);
}

void GHOST_OpenHMDManager::getLeftEyeGLProjectionMatrix(float mat[16]) const
{
	if (!m_available) {
		return;
	}

	ohmd_device_getf(m_device, OHMD_LEFT_EYE_GL_PROJECTION_MATRIX, mat);
}

void GHOST_OpenHMDManager::getRightEyeGLProjectionMatrix(float mat[16]) const
{
	if (!m_available) {
		return;
	}

	ohmd_device_getf(m_device, OHMD_RIGHT_EYE_GL_PROJECTION_MATRIX, mat);
}

void GHOST_OpenHMDManager::getPositionVector(float position[3]) const
{
	if (!m_available) {
		return;
	}

	ohmd_device_getf(m_device, OHMD_POSITION_VECTOR, position);
}

float GHOST_OpenHMDManager::getScreenHorizontalSize() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_SCREEN_HORIZONTAL_SIZE, &val);
	return val;
}

float GHOST_OpenHMDManager::getScreenVerticalSize() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_SCREEN_VERTICAL_SIZE, &val);
	return val;
}

float GHOST_OpenHMDManager::getLensHorizontalSeparation() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_LENS_HORIZONTAL_SEPARATION, &val);
	return val;

}

float GHOST_OpenHMDManager::getLensVerticalPosition() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_LENS_VERTICAL_POSITION, &val);
	return val;
}

float GHOST_OpenHMDManager::getLeftEyeFOV() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_LEFT_EYE_FOV, &val);
	return val;
}

float GHOST_OpenHMDManager::getLeftEyeAspectRatio() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_LEFT_EYE_ASPECT_RATIO, &val);
	return val;
}

float GHOST_OpenHMDManager::getRightEyeFOV() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_RIGHT_EYE_FOV, &val);
	return val;
}

float GHOST_OpenHMDManager::getRightEyeAspectRatio() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_RIGHT_EYE_ASPECT_RATIO, &val);
	return val;
}

float GHOST_OpenHMDManager::getEyeIPD() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_EYE_IPD, &val);
	return val;
}

float GHOST_OpenHMDManager::getProjectionZFar() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_PROJECTION_ZFAR, &val);
	return val;
}

float GHOST_OpenHMDManager::getProjectionZNear() const
{
	if (!m_available) {
		return -1;
	}

	float val = -1;
	ohmd_device_getf(m_device, OHMD_PROJECTION_ZNEAR, &val);
	return val;
}

void GHOST_OpenHMDManager::getDistortion(float distortion[6]) const
{
	if (!m_available) {
		return;
	}

	ohmd_device_getf(m_device, OHMD_DISTORTION_K, distortion);
}

int GHOST_OpenHMDManager::getScreenHorizontalResolution() const
{
	if (!m_available) {
		return -1;
	}

	int val = -1;
	ohmd_device_geti(m_device, OHMD_SCREEN_HORIZONTAL_RESOLUTION, &val);
	return val;
}

int GHOST_OpenHMDManager::getScreenVerticalResolution() const
{
	if (!m_available) {
		return -1;
	}

	int val = -1;
	ohmd_device_geti(m_device, OHMD_SCREEN_VERTICAL_RESOLUTION, &val);
	return val;
}

bool GHOST_OpenHMDManager::setEyeIPD(float val)
{
	if (!m_available) {
		return false;
	}

	return ohmd_device_setf(m_device, OHMD_EYE_IPD, &val);
}

bool GHOST_OpenHMDManager::setProjectionZFar(float val)
{
	if (!m_available) {
		return false;
	}

	return ohmd_device_setf(m_device, OHMD_PROJECTION_ZFAR, &val);
}

bool GHOST_OpenHMDManager::setProjectionZNear(float val)
{
	if (!m_available) {
		return false;
	}

	return ohmd_device_setf(m_device, OHMD_PROJECTION_ZNEAR, &val);
}

ohmd_context *GHOST_OpenHMDManager::getOpenHMDContext()
{
	return m_context;
}

ohmd_device *GHOST_OpenHMDManager::getOpenHMDDevice()
{
	return m_device;
}

const int GHOST_OpenHMDManager::getDeviceIndex()
{
	return m_deviceIndex;
}
