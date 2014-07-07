/*
 * Copyright (C) 2011 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ipxe/version.h>
#include <ipxe/efi/efi.h>
#include <ipxe/efi/Protocol/DriverBinding.h>
#include <ipxe/efi/Protocol/ComponentName2.h>
#include <ipxe/efi/Protocol/DevicePath.h>
#include <ipxe/efi/efi_strings.h>
#include <ipxe/efi/efi_driver.h>

/** @file
 *
 * EFI driver interface
 *
 */

static EFI_DRIVER_BINDING_PROTOCOL efi_driver_binding;

/** EFI driver binding protocol GUID */
static EFI_GUID efi_driver_binding_protocol_guid
	= EFI_DRIVER_BINDING_PROTOCOL_GUID;

/** EFI component name protocol GUID */
static EFI_GUID efi_component_name2_protocol_guid
	= EFI_COMPONENT_NAME2_PROTOCOL_GUID;

/** EFI device path protocol GUID */
static EFI_GUID efi_device_path_protocol_guid
	= EFI_DEVICE_PATH_PROTOCOL_GUID;

/** List of controlled EFI devices */
static LIST_HEAD ( efi_devices );

/**
 * Find end of device path
 *
 * @v path		Path to device
 * @ret path_end	End of device path
 */
EFI_DEVICE_PATH_PROTOCOL * efi_devpath_end ( EFI_DEVICE_PATH_PROTOCOL *path ) {

	while ( path->Type != END_DEVICE_PATH_TYPE ) {
		path = ( ( ( void * ) path ) +
			 /* There's this amazing new-fangled thing known as
			  * a UINT16, but who wants to use one of those? */
			 ( ( path->Length[1] << 8 ) | path->Length[0] ) );
	}

	return path;
}

/**
 * Find EFI device
 *
 * @v device		EFI device handle
 * @ret efidev		EFI device, or NULL if not found
 */
static struct efi_device * efidev_find ( EFI_HANDLE device ) {
	struct efi_device *efidev;

	/* Look for an existing EFI device */
	list_for_each_entry ( efidev, &efi_devices, dev.siblings ) {
		if ( efidev->device == device )
			return efidev;
	}

	return NULL;
}

/**
 * Get parent EFI device
 *
 * @v dev		Generic device
 * @ret efidev		Parent EFI device, or NULL
 */
struct efi_device * efidev_parent ( struct device *dev ) {
	struct device *parent = dev->parent;
	struct efi_device *efidev;

	/* Check that parent exists and is an EFI device */
	if ( ! parent )
		return NULL;
	if ( parent->desc.bus_type != BUS_TYPE_EFI )
		return NULL;

	/* Get containing EFI device */
	efidev = container_of ( parent, struct efi_device, dev );
	return efidev;
}

/**
 * Add EFI device as child of EFI device
 *
 * @v efidev		EFI device
 * @v device		EFI child device handle
 * @ret efirc		EFI status code
 */
int efidev_child_add ( struct efi_device *efidev, EFI_HANDLE device ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	void *devpath;
	EFI_STATUS efirc;
	int rc;

	/* Re-open the device path protocol */
	if ( ( efirc = bs->OpenProtocol ( efidev->device,
					  &efi_device_path_protocol_guid,
					  &devpath,
					  efi_image_handle, device,
					  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
					  ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( efidev->device, "EFIDRV %p %s could not add child",
		       efidev->device, efi_devpath_text ( efidev->path ) );
		DBGC ( efidev->device, " %p %s: %s\n", device,
		       efi_handle_devpath_text ( device ), strerror ( rc ) );
		return rc;
	}

	DBGC2 ( efidev->device, "EFIDRV %p %s added child",
		efidev->device, efi_devpath_text ( efidev->path ) );
	DBGC2 ( efidev->device, " %p %s\n",
		device, efi_handle_devpath_text ( device ) );
	return 0;
}

/**
 * Remove EFI device as child of EFI device
 *
 * @v efidev		EFI device
 * @v device		EFI child device handle
 * @ret efirc		EFI status code
 */
void efidev_child_del ( struct efi_device *efidev, EFI_HANDLE device ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;

	bs->CloseProtocol ( efidev->device,
			    &efi_device_path_protocol_guid,
			    efi_image_handle, device );
	DBGC2 ( efidev->device, "EFIDRV %p %s removed child",
		efidev->device, efi_devpath_text ( efidev->path ) );
	DBGC2 ( efidev->device, " %p %s\n",
		device, efi_handle_devpath_text ( device ) );
}

/**
 * Check to see if driver supports a device
 *
 * @v driver		EFI driver
 * @v device		EFI device
 * @v child		Path to child device, if any
 * @ret efirc		EFI status code
 */
static EFI_STATUS EFIAPI
efi_driver_supported ( EFI_DRIVER_BINDING_PROTOCOL *driver __unused,
		       EFI_HANDLE device, EFI_DEVICE_PATH_PROTOCOL *child ) {
	struct efi_driver *efidrv;
	int rc;

	DBGCP ( device, "EFIDRV %p %s DRIVER_SUPPORTED",
		device, efi_handle_devpath_text ( device ) );
	if ( child )
		DBGCP ( device, " (child %s)", efi_devpath_text ( child ) );
	DBGCP ( device, "\n" );

	/* Do nothing if we are already driving this device */
	if ( efidev_find ( device ) != NULL ) {
		DBGCP ( device, "EFIDRV %p %s is already started\n",
			device, efi_handle_devpath_text ( device ) );
		return EFI_ALREADY_STARTED;
	}

	/* Look for a driver claiming to support this device */
	for_each_table_entry ( efidrv, EFI_DRIVERS ) {
		if ( ( rc = efidrv->supported ( device ) ) == 0 ) {
			DBGC ( device, "EFIDRV %p %s has driver \"%s\"\n",
			       device, efi_handle_devpath_text ( device ),
			       efidrv->name );
			return 0;
		}
	}
	DBGCP ( device, "EFIDRV %p %s has no driver\n",
		device, efi_handle_devpath_text ( device ) );

	return EFI_UNSUPPORTED;
}

/**
 * Attach driver to device
 *
 * @v driver		EFI driver
 * @v device		EFI device
 * @v child		Path to child device, if any
 * @ret efirc		EFI status code
 */
static EFI_STATUS EFIAPI
efi_driver_start ( EFI_DRIVER_BINDING_PROTOCOL *driver __unused,
		   EFI_HANDLE device, EFI_DEVICE_PATH_PROTOCOL *child ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	struct efi_driver *efidrv;
	struct efi_device *efidev;
	union {
		EFI_DEVICE_PATH_PROTOCOL *devpath;
		void *interface;
	} devpath;
	EFI_STATUS efirc;
	int rc;

	DBGC ( device, "EFIDRV %p %s DRIVER_START",
	       device, efi_handle_devpath_text ( device ) );
	if ( child )
		DBGC ( device, " (child %s)", efi_devpath_text ( child ) );
	DBGC ( device, "\n" );

	/* Do nothing if we are already driving this device */
	efidev = efidev_find ( device );
	if ( efidev ) {
		DBGCP ( device, "EFIDRV %p %s is already started\n",
			device, efi_devpath_text ( efidev->path ) );
		efirc = EFI_ALREADY_STARTED;
		goto err_already_started;
	}

	/* Allocate and initialise structure */
	efidev = zalloc ( sizeof ( *efidev ) );
	if ( ! efidev ) {
		efirc = EFI_OUT_OF_RESOURCES;
		goto err_alloc;
	}
	efidev->device = device;
	efidev->dev.desc.bus_type = BUS_TYPE_EFI;
	INIT_LIST_HEAD ( &efidev->dev.children );
	list_add ( &efidev->dev.siblings, &efi_devices );

	/* Open device path protocol */
	if ( ( efirc = bs->OpenProtocol ( device,
					  &efi_device_path_protocol_guid,
					  &devpath.interface,
					  efi_image_handle, device,
					  EFI_OPEN_PROTOCOL_BY_DRIVER ) ) != 0){
		DBGCP ( device, "EFIDRV %p %s has no device path\n",
			device, efi_handle_devpath_text ( device ) );
		goto err_no_device_path;
	}
	efidev->path = devpath.devpath;

	/* Try to start this device */
	for_each_table_entry ( efidrv, EFI_DRIVERS ) {
		if ( ( rc = efidrv->start ( efidev ) ) == 0 ) {
			efidev->driver = efidrv;
			DBGC ( device, "EFIDRV %p %s using driver \"%s\"\n",
			       device, efi_devpath_text ( efidev->path ),
			       efidev->driver->name );
			return 0;
		}
		DBGC ( device, "EFIDRV %p %s could not start driver \"%s\": "
		       "%s\n", device, efi_devpath_text ( efidev->path ),
		       efidrv->name, strerror ( rc ) );
	}
	efirc = EFI_UNSUPPORTED;

	bs->CloseProtocol ( device, &efi_device_path_protocol_guid,
			    efi_image_handle, device );
 err_no_device_path:
	list_del ( &efidev->dev.siblings );
	free ( efidev );
 err_alloc:
 err_already_started:
	return efirc;
}

/**
 * Detach driver from device
 *
 * @v driver		EFI driver
 * @v device		EFI device
 * @v pci		PCI device
 * @v num_children	Number of child devices
 * @v children		List of child devices
 * @ret efirc		EFI status code
 */
static EFI_STATUS EFIAPI
efi_driver_stop ( EFI_DRIVER_BINDING_PROTOCOL *driver __unused,
		  EFI_HANDLE device, UINTN num_children,
		  EFI_HANDLE *children ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	struct efi_driver *efidrv;
	struct efi_device *efidev;
	UINTN i;

	DBGC ( device, "EFIDRV %p %s DRIVER_STOP",
	       device, efi_handle_devpath_text ( device ) );
	for ( i = 0 ; i < num_children ; i++ ) {
		DBGC ( device, "%s%p %s", ( i ? ", " : " child " ),
		       children[i], efi_handle_devpath_text ( children[i] ) );
	}
	DBGC ( device, "\n" );

	/* Do nothing unless we are driving this device */
	efidev = efidev_find ( device );
	if ( ! efidev ) {
		DBGCP ( device, "EFIDRV %p %s is not started\n",
			device, efi_devpath_text ( efidev->path ) );
		return 0;
	}

	/* Stop this device */
	efidrv = efidev->driver;
	assert ( efidrv != NULL );
	efidrv->stop ( efidev );
	bs->CloseProtocol ( efidev->device, &efi_device_path_protocol_guid,
			    efi_image_handle, efidev->device );
	list_del ( &efidev->dev.siblings );
	free ( efidev );

	return 0;
}

/** EFI driver binding protocol */
static EFI_DRIVER_BINDING_PROTOCOL efi_driver_binding = {
	.Supported = efi_driver_supported,
	.Start = efi_driver_start,
	.Stop = efi_driver_stop,
};

/**
 * Look up driver name
 *
 * @v wtf		Component name protocol
 * @v language		Language to use
 * @v driver_name	Driver name to fill in
 * @ret efirc		EFI status code
 */
static EFI_STATUS EFIAPI
efi_driver_name ( EFI_COMPONENT_NAME2_PROTOCOL *wtf __unused,
		  CHAR8 *language __unused, CHAR16 **driver_name ) {
	const wchar_t *name;

	name = ( product_wname[0] ? product_wname : build_wname );
	*driver_name = ( ( wchar_t * ) name );
	return 0;
}

/**
 * Look up controller name
 *
 * @v wtf		Component name protocol
 * @v device		Device
 * @v child		Child device, or NULL
 * @v language		Language to use
 * @v driver_name	Device name to fill in
 * @ret efirc		EFI status code
 */
static EFI_STATUS EFIAPI
efi_driver_controller_name ( EFI_COMPONENT_NAME2_PROTOCOL *wtf __unused,
			     EFI_HANDLE device, EFI_HANDLE child,
			     CHAR8 *language, CHAR16 **controller_name ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	union {
		EFI_COMPONENT_NAME2_PROTOCOL *name2;
		void *interface;
	} name2;
	EFI_STATUS efirc;

	/* Delegate to the EFI_COMPONENT_NAME2_PROTOCOL instance
	 * installed on child handle, if present.
	 */
	if ( ( child != NULL ) &&
	     ( ( efirc = bs->OpenProtocol (
			  child, &efi_component_name2_protocol_guid,
			  &name2.interface, NULL, NULL,
			  EFI_OPEN_PROTOCOL_GET_PROTOCOL ) ) == 0 ) ) {
		return name2.name2->GetControllerName ( name2.name2, device,
							child, language,
							controller_name );
	}

	/* Otherwise, let EFI use the default Device Path Name */
	return EFI_UNSUPPORTED;
}

/** EFI component name protocol */
static EFI_COMPONENT_NAME2_PROTOCOL efi_wtf = {
	.GetDriverName = efi_driver_name,
	.GetControllerName = efi_driver_controller_name,
	.SupportedLanguages = "en",
};

/**
 * Install EFI driver
 *
 * @ret rc		Return status code
 */
int efi_driver_install ( void ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	EFI_STATUS efirc;
	int rc;

	/* Calculate driver version number.  We use the build
	 * timestamp (in seconds since the Epoch) shifted right by six
	 * bits: this gives us an approximately one-minute resolution
	 * and a scheme which will last until the year 10680.
	 */
	efi_driver_binding.Version = ( build_timestamp >> 6 );

	/* Install protocols on image handle */
	efi_driver_binding.ImageHandle = efi_image_handle;
	efi_driver_binding.DriverBindingHandle = efi_image_handle;
	if ( ( efirc = bs->InstallMultipleProtocolInterfaces (
			&efi_image_handle,
			&efi_driver_binding_protocol_guid, &efi_driver_binding,
			&efi_component_name2_protocol_guid, &efi_wtf,
			NULL ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( &efi_driver_binding, "EFIDRV could not install "
		       "protocols: %s\n", strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Uninstall EFI driver
 *
 */
void efi_driver_uninstall ( void ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;

	/* Uninstall protocols */
	bs->UninstallMultipleProtocolInterfaces (
		efi_image_handle,
		&efi_driver_binding_protocol_guid, &efi_driver_binding,
		&efi_component_name2_protocol_guid, &efi_wtf, NULL );
}

/**
 * Try to connect EFI driver
 *
 * @v device		EFI device
 * @ret rc		Return status code
 */
static int efi_driver_connect ( EFI_HANDLE device ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	EFI_HANDLE drivers[2] =
		{ efi_driver_binding.DriverBindingHandle, NULL };
	EFI_STATUS efirc;
	int rc;

	/* Check if we want to drive this device */
	if ( ( efirc = efi_driver_supported ( &efi_driver_binding, device,
					      NULL ) ) != 0 ) {
		/* Not supported; not an error */
		return 0;
	}

	/* Disconnect any existing drivers */
	DBGC ( device, "EFIDRV %p %s disconnecting existing drivers\n",
	       device, efi_handle_devpath_text ( device ) );
	bs->DisconnectController ( device, NULL, NULL );

	/* Connect our driver */
	DBGC ( device, "EFIDRV %p %s connecting new drivers\n",
	       device, efi_handle_devpath_text ( device ) );
	if ( ( efirc = bs->ConnectController ( device, drivers, NULL,
					       FALSE ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( device, "EFIDRV %p %s could not connect new drivers: "
		       "%s\n", device, efi_handle_devpath_text ( device ),
		       strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Try to disconnect EFI driver
 *
 * @v device		EFI device
 * @ret rc		Return status code
 */
static int efi_driver_disconnect ( EFI_HANDLE device ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;

	/* Disconnect our driver */
	bs->DisconnectController ( device,
				   efi_driver_binding.DriverBindingHandle,
				   NULL );
	return 0;
}

/**
 * Reconnect original EFI driver
 *
 * @v device		EFI device
 * @ret rc		Return status code
 */
static int efi_driver_reconnect ( EFI_HANDLE device ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;

	/* Reconnect any available driver */
	bs->ConnectController ( device, NULL, NULL, FALSE );

	return 0;
}

/**
 * Connect/disconnect EFI driver from all handles
 *
 * @v method		Connect/disconnect method
 * @ret rc		Return status code
 */
static int efi_driver_handles ( int ( * method ) ( EFI_HANDLE handle ) ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	EFI_HANDLE *handles;
	UINTN num_handles;
	EFI_STATUS efirc;
	UINTN i;
	int rc;

	/* Enumerate all handles */
	if ( ( efirc = bs->LocateHandleBuffer ( AllHandles, NULL, NULL,
						&num_handles,
						&handles ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( &efi_driver_binding, "EFIDRV could not list handles: "
		       "%s\n", strerror ( rc ) );
		goto err_locate;
	}

	/* Connect/disconnect driver from all handles */
	for ( i = 0 ; i < num_handles ; i++ ) {
		if ( ( rc = method ( handles[i] ) ) != 0 )
			goto err_method;
	}

	/* Success */
	rc = 0;

 err_method:
	bs->FreePool ( handles );
 err_locate:
	return rc;
}

/**
 * Connect EFI driver to all possible devices
 *
 * @ret rc		Return status code
 */
int efi_driver_connect_all ( void ) {

	DBGC ( &efi_driver_binding, "EFIDRV connecting our drivers\n" );
	return efi_driver_handles ( efi_driver_connect );
}

/**
 * Disconnect EFI driver from all possible devices
 *
 * @ret rc		Return status code
 */
void efi_driver_disconnect_all ( void ) {

	DBGC ( &efi_driver_binding, "EFIDRV disconnecting our drivers\n" );
	efi_driver_handles ( efi_driver_disconnect );
}

/**
 * Reconnect original EFI drivers to all possible devices
 *
 * @ret rc		Return status code
 */
void efi_driver_reconnect_all ( void ) {

	DBGC ( &efi_driver_binding, "EFIDRV reconnecting old drivers\n" );
	efi_driver_handles ( efi_driver_reconnect );
}
