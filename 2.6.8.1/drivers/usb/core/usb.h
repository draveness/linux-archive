/* Functions local to drivers/usb/core/ */

extern void usb_create_sysfs_dev_files (struct usb_device *dev);
extern void usb_remove_sysfs_dev_files (struct usb_device *dev);
extern void usb_create_sysfs_intf_files (struct usb_interface *intf);
extern void usb_remove_sysfs_intf_files (struct usb_interface *intf);
extern int usb_probe_interface (struct device *dev);
extern int usb_unbind_interface (struct device *dev);

extern void usb_disable_endpoint (struct usb_device *dev, unsigned int epaddr);
extern void usb_disable_interface (struct usb_device *dev,
		struct usb_interface *intf);
extern void usb_disable_device (struct usb_device *dev, int skip_ep0);

extern void usb_enable_endpoint (struct usb_device *dev,
		struct usb_endpoint_descriptor *epd);
extern void usb_enable_interface (struct usb_device *dev,
		struct usb_interface *intf);

extern int usb_get_device_descriptor(struct usb_device *dev,
		unsigned int size);
extern int usb_set_configuration(struct usb_device *dev, int configuration);

extern void usb_set_device_state(struct usb_device *udev,
		enum usb_device_state new_state);

/* for labeling diagnostics */
extern const char *usbcore_name;
