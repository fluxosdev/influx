/* USB core: the device model shared by the host controller drivers (xhci.c,
 * ehci.c) and the class drivers (hubs in usb.c, hid.c, msc.c). */
#pragma once
#include "kernel.h"
#include "sched.h"
#include "fs.h"

#ifndef EPIPE
#define EPIPE     32
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

/* speeds use the xHCI protocol speed IDs */
enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4 };

/* bmRequestType */
#define USB_DIR_IN       0x80
#define USB_TYPE_CLASS   0x20
#define USB_RECIP_INTF   0x01
#define USB_RECIP_EP     0x02
#define USB_RECIP_OTHER  0x03
/* standard requests */
#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE     11
/* descriptor types */
#define USB_DT_DEVICE     1
#define USB_DT_CONFIG     2
#define USB_DT_STRING     3
#define USB_DT_INTERFACE  4
#define USB_DT_ENDPOINT   5
#define USB_DT_HID        0x21
#define USB_DT_REPORT     0x22
#define USB_DT_HUB        0x29
#define USB_DT_SS_HUB     0x2A
#define USB_DT_SS_EP_COMP 0x30
/* endpoint transfer types */
enum { USB_EP_CONTROL, USB_EP_ISOC, USB_EP_BULK, USB_EP_INTR };

struct usb_device_desc {
    uint8_t bLength, bDescriptorType; uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} PACKED;
struct usb_config_desc { uint8_t bLength, bDescriptorType; uint16_t wTotalLength; uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower; } PACKED;
struct usb_interface_desc { uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface; } PACKED;
struct usb_endpoint_desc { uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes; uint16_t wMaxPacketSize; uint8_t bInterval; } PACKED;

struct usb_endpoint {
    uint8_t addr;         /* bEndpointAddress: number | USB_DIR_IN; 0 for the control endpoint */
    uint8_t type;         /* USB_EP_* */
    uint16_t mps;         /* max packet size */
    uint8_t mult;         /* high-bandwidth high-speed: extra transactions per microframe */
    uint8_t burst;        /* SuperSpeed: max burst - 1 */
    uint8_t interval;     /* bInterval as the descriptor gives it */
    void *hcpriv;
};

#define USB_MAX_INTF 8
#define USB_MAX_EP   8
struct usb_device;
struct usb_driver;
struct usb_interface {
    struct usb_device *dev;
    uint8_t num, cls, subcls, proto;
    uint8_t alt;                             /* current alternate setting */
    int nep;
    struct usb_endpoint ep[USB_MAX_EP];
    const uint8_t *extra; int extra_len;     /* class descriptors following the interface descriptor */
    const struct usb_driver *driver;
    void *drvdata;
};

struct usb_hcd;
struct usb_hub;
struct usb_device {
    struct usb_hcd *hcd;
    struct usb_hub *parent;   /* the hub it is plugged into (a root hub for root ports) */
    int port;                 /* port on that hub, 1-based */
    int root_port;            /* root port at the top of its branch */
    uint32_t route;           /* xHCI route string: hub port per tier below the root */
    int depth;                /* hubs between it and the root hub */
    int speed;
    uint8_t addr;
    struct usb_device *tt_hub; int tt_port;   /* high-speed hub doing split transactions for a LS/FS device */
    int hub_ports, hub_ttt;   /* set while it is configured as a hub */
    struct usb_device_desc desc;
    uint8_t *config; int config_len;
    int nintf;
    struct usb_interface intf[USB_MAX_INTF];
    struct usb_endpoint ep0;
    struct mutex ctl_lock;    /* one control transfer at a time */
    char manufacturer[48], product[48], serial[48];
    struct usb_hub *hub;      /* when it is a hub */
    volatile bool gone;
    void *hcpriv;
};

/* A transfer. The completion callback runs with interrupts disabled and may
 * resubmit; without one, the submitter waits (usb_control, usb_transfer).
 * An isochronous transfer is a run of packets, one per service interval; it
 * completes with status 0 even when the controller missed some of them. */
#define USB_PENDING 1
struct usb_xfer {
    struct usb_device *dev;
    struct usb_endpoint *ep;            /* &dev->ep0 for control transfers */
    uint8_t setup[8];
    void *buf; uint32_t len;
    int npackets;                       /* isochronous: packets of packet_len[] bytes, back to back in buf */
    const uint16_t *packet_len;
    uint32_t actual;
    volatile int status;                /* USB_PENDING, then 0 or -errno */
    void (*done)(struct usb_xfer *x);
    void *ctx;
    struct semaphore sem;
    /* core and controller private */
    void *data;                         /* buf, or its bounce buffer: HHDM memory the controller can reach */
    uint64_t bounce; uint32_t bounce_pages;
    struct usb_xfer *next;
    void *hcpriv;
    int hc_first, hc_last, hc_ntrb;
};

struct usb_port_status { bool connected, enabled, changed; int speed; };

struct usb_hcd_ops {
    /* root hub ports, 1-based; port_status clears the change bits it reports */
    void (*port_status)(struct usb_hcd *h, int port, struct usb_port_status *st);
    int (*port_reset)(struct usb_hcd *h, int port);          /* 0 once the port is enabled */
    /* devices */
    int (*enable_device)(struct usb_device *d);   /* give it an address, settle ep0.mps */
    int (*configure)(struct usb_device *d);       /* after SET_CONFIGURATION: the interfaces' endpoints */
    int (*hub_config)(struct usb_device *d);      /* optional: d->hub_ports/hub_ttt are set */
    /* optional: i->ep now holds the new alternate setting's endpoints, old[] the previous ones */
    int (*set_interface)(struct usb_device *d, struct usb_interface *i, const struct usb_endpoint *old, int nold);
    void (*free_device)(struct usb_device *d);    /* fail its transfers, release controller state */
    /* transfers */
    int (*submit)(struct usb_xfer *x);
    void (*abort)(struct usb_xfer *x);            /* thread context: complete a timed-out transfer */
    int (*reset_ep)(struct usb_device *d, struct usb_endpoint *ep);   /* after a stall; fails queued transfers */
};

struct usb_hcd {
    const char *name;
    const struct usb_hcd_ops *ops;
    void *priv;
    struct pci_device *pci;
    int nports;
    bool dma32;               /* controller only reaches the low 4 GiB */
    int index;
    struct usb_hub *root;
};

struct usb_driver {
    const char *name;
    bool (*probe)(struct usb_interface *i);
    void (*disconnect)(struct usb_interface *i);
};

void usb_init(void);
void usb_register_hcd(struct usb_hcd *h);        /* controller is running: scan its root ports */
void usb_hub_event(void);                        /* some port changed (any context) */
void usb_complete(struct usb_xfer *x, int status, uint32_t actual);   /* controller -> core, interrupts off */
int usb_submit(struct usb_xfer *x);
int usb_control(struct usb_device *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len);   /* bytes or -errno */
int usb_transfer(struct usb_device *d, struct usb_endpoint *ep, void *buf, uint32_t len, int timeout_ms);   /* bytes or -errno; clears a stall */
int usb_clear_halt(struct usb_device *d, struct usb_endpoint *ep);
void *usb_buf_alloc(struct usb_device *d, size_t n);   /* zeroed, never needs a bounce buffer */
void usb_buf_free(struct usb_device *d, void *p);
struct usb_endpoint *usb_find_ep(struct usb_interface *i, int type, bool in);
struct usb_endpoint *usb_ep_by_addr(struct usb_interface *i, uint8_t addr);
int usb_set_interface(struct usb_interface *i, int alt);   /* -ENOTSUP if the controller has no isochronous support */
const uint8_t *usb_find_desc(struct usb_interface *i, uint8_t type);
bool usb_ep_in(const struct usb_xfer *x);
const char *usb_speed_name(int speed);
void usb_print(bool verbose, void (*line)(void *ctx, const char *s), void *ctx);

/* host controllers and class drivers */
void xhci_init(void);
void ehci_init(void);
extern const struct usb_driver usb_hid_driver, usb_msc_driver, usb_audio_driver;
