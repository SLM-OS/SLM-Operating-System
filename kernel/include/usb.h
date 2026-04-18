/*
 * usb.h - SLM-OS USB core public API
 *
 * Phase 1 of #266 (Jetson USB networking). Platform-agnostic USB core
 * on top of a minimal host-controller-driver (HCD) abstraction. A URB
 * (USB Request Block) is the transfer primitive the HCD implements
 * against and class drivers build upon — modelled after the same idea
 * in Linux's drivers/usb/core/.
 *
 * Scope is deliberately narrow (docs/jetson-usb-networking-plan.md §6):
 *   - Single device at boot; no hubs, no dynamic topology.
 *   - USB 2.0 high/full speed only; no SuperSpeed handling here.
 *   - CDC-ECM is the only class the Phase 2 glue will target.
 *   - No hot-plug beyond connect-at-boot enumeration.
 *
 * The header compiles on every platform (no platform.h dependencies).
 * The implementation file usb_core.c is always linked, but HCD
 * drivers (Phase 3A XHCI) are platform-gated at registration time.
 */

#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* USB standard constants                                                      */
/* -------------------------------------------------------------------------- */

/* Transfer directions (bit 7 of bmRequestType / bEndpointAddress) */
#define USB_DIR_OUT                 0x00u
#define USB_DIR_IN                  0x80u

/* Request types (bmRequestType bits 6:5) */
#define USB_TYPE_STANDARD           (0x00u << 5)
#define USB_TYPE_CLASS              (0x01u << 5)
#define USB_TYPE_VENDOR             (0x02u << 5)
#define USB_TYPE_MASK               (0x03u << 5)

/* Recipients (bmRequestType bits 4:0) */
#define USB_RECIP_DEVICE            0x00u
#define USB_RECIP_INTERFACE         0x01u
#define USB_RECIP_ENDPOINT          0x02u
#define USB_RECIP_OTHER             0x03u

/* Standard device requests (bRequest) */
#define USB_REQ_GET_STATUS          0x00u
#define USB_REQ_CLEAR_FEATURE       0x01u
#define USB_REQ_SET_FEATURE         0x03u
#define USB_REQ_SET_ADDRESS         0x05u
#define USB_REQ_GET_DESCRIPTOR      0x06u
#define USB_REQ_SET_DESCRIPTOR      0x07u
#define USB_REQ_GET_CONFIGURATION   0x08u
#define USB_REQ_SET_CONFIGURATION   0x09u
#define USB_REQ_GET_INTERFACE       0x0Au
#define USB_REQ_SET_INTERFACE       0x0Bu

/* Descriptor types (high byte of wValue for GET_DESCRIPTOR) */
#define USB_DT_DEVICE               0x01u
#define USB_DT_CONFIG               0x02u
#define USB_DT_STRING               0x03u
#define USB_DT_INTERFACE            0x04u
#define USB_DT_ENDPOINT             0x05u
#define USB_DT_INTERFACE_ASSOC      0x0Bu
#define USB_DT_CS_INTERFACE         0x24u   /* class-specific */
#define USB_DT_CS_ENDPOINT          0x25u   /* class-specific */

/* Endpoint transfer types (bmAttributes bits 1:0) */
#define USB_XFER_CONTROL            0x00u
#define USB_XFER_ISOC               0x01u
#define USB_XFER_BULK               0x02u
#define USB_XFER_INTERRUPT          0x03u
#define USB_XFER_TYPE_MASK          0x03u

/* Device states */
enum usb_device_state {
    USB_STATE_DETACHED = 0,
    USB_STATE_ATTACHED,      /* port sees connect */
    USB_STATE_POWERED,       /* VBUS asserted */
    USB_STATE_DEFAULT,       /* reset complete, address 0 */
    USB_STATE_ADDRESS,       /* SET_ADDRESS succeeded */
    USB_STATE_CONFIGURED,    /* SET_CONFIGURATION succeeded */
};

/* Speeds */
enum usb_speed {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW,           /* 1.5 Mbps */
    USB_SPEED_FULL,          /* 12 Mbps */
    USB_SPEED_HIGH,          /* 480 Mbps — only target speed for Phase 3A */
    USB_SPEED_SUPER,         /* 5 Gbps — out of scope */
};

/* URB transfer status */
enum usb_urb_status {
    USB_URB_OK = 0,
    USB_URB_PENDING,         /* queued or in flight */
    USB_URB_CANCELLED,
    USB_URB_STALL,           /* endpoint halted */
    USB_URB_TIMEOUT,
    USB_URB_SHORT,           /* short packet received (may still be OK) */
    USB_URB_IO_ERROR,
};

/* -------------------------------------------------------------------------- */
/* Standard descriptor layouts (packed USB wire format)                        */
/* -------------------------------------------------------------------------- */

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

struct usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

_Static_assert(sizeof(struct usb_device_descriptor)    == 18, "device desc size");
_Static_assert(sizeof(struct usb_config_descriptor)    ==  9, "config desc size");
_Static_assert(sizeof(struct usb_interface_descriptor) ==  9, "iface desc size");
_Static_assert(sizeof(struct usb_endpoint_descriptor)  ==  7, "endpoint desc size");
_Static_assert(sizeof(struct usb_setup_packet)         ==  8, "setup packet size");

/* -------------------------------------------------------------------------- */
/* Endpoint + device model                                                     */
/* -------------------------------------------------------------------------- */

/* Phase-1 caps — grow only if the CDC-ECM use case needs it. */
#define USB_MAX_ENDPOINTS_PER_DEV   8
#define USB_MAX_INTERFACES_PER_DEV  4
#define USB_MAX_CONFIG_DESC_BYTES   256

struct usb_endpoint {
    bool     valid;
    uint8_t  address;           /* USB_DIR_IN|OUT | ep number 1..15 */
    uint8_t  attributes;        /* transfer type in low 2 bits */
    uint16_t max_packet;
    uint8_t  interval;          /* interrupt/iso polling, else 0 */
};

struct usb_interface {
    bool     valid;
    uint8_t  number;
    uint8_t  alt_setting;
    uint8_t  num_endpoints;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    /* Endpoint indices into usb_device.endpoints[]; -1 if unset. */
    int8_t   ep_index[USB_MAX_ENDPOINTS_PER_DEV];
};

struct usb_hcd;     /* forward */
struct usb_urb;     /* forward */

struct usb_device {
    const struct usb_hcd *hcd;
    void                 *hcd_private;   /* HCD per-device state */
    uint8_t               address;       /* USB bus address after SET_ADDRESS */
    enum usb_speed        speed;
    enum usb_device_state state;
    uint8_t               port;          /* root-port number on the HCD */
    struct usb_device_descriptor    dev_desc;
    uint8_t                         raw_config[USB_MAX_CONFIG_DESC_BYTES];
    uint16_t                        raw_config_len;
    struct usb_interface            ifaces[USB_MAX_INTERFACES_PER_DEV];
    struct usb_endpoint             endpoints[USB_MAX_ENDPOINTS_PER_DEV];
    uint8_t                         current_config;
};

/* -------------------------------------------------------------------------- */
/* URB (USB Request Block)                                                     */
/* -------------------------------------------------------------------------- */

/*
 * A URB is a single transfer request. The submitter fills it in, calls
 * usb_submit_urb(), and either blocks in usb_wait_urb() or registers a
 * completion callback. Buffer ownership stays with the submitter for
 * the lifetime of the transfer; the HCD must not free it.
 *
 * For control transfers, `setup` holds the 8-byte SETUP packet and
 * `buffer`/`length` is the optional data stage payload.
 *
 * DMA discipline (Jetson + Pi 5): buffers must be either cacheable
 * with explicit cache maintenance by the controller driver, or
 * allocated from non-cacheable memory (ncmem_alloc). Phase 3A XHCI
 * will document its exact discipline; the core does not enforce it.
 */
typedef void (*usb_urb_complete_fn)(struct usb_urb *urb);

struct usb_urb {
    struct usb_device        *dev;
    uint8_t                   endpoint;       /* USB_DIR_IN|OUT | ep num */
    uint8_t                   transfer_type;  /* USB_XFER_* */
    struct usb_setup_packet   setup;          /* control only */
    void                     *buffer;         /* data stage or bulk/int */
    uint32_t                  length;         /* requested bytes */
    uint32_t                  actual_length;  /* bytes transferred */
    enum usb_urb_status       status;
    usb_urb_complete_fn       complete;       /* optional; may be NULL */
    void                     *context;        /* opaque submitter state */

    /* HCD may stash per-URB state here (TRB pointers, ring slot, etc). */
    void                     *hcd_private;
};

/* -------------------------------------------------------------------------- */
/* Host controller driver (HCD) abstraction                                    */
/* -------------------------------------------------------------------------- */

struct usb_hcd {
    const char *name;

    /*
     * Bring the controller out of reset, initialize rings/queues, and
     * make it ready to accept URB submissions. Must not enumerate
     * devices — that happens in usb_core_enumerate() against the HCD's
     * port state.
     */
    int  (*start)(void);

    /*
     * Poll root-port status. Returns true and writes speed / connection
     * state if a device is present on the indicated port. Phase 3A
     * uses port 0 only; hubs/multi-port deferred to post-Phase 5.
     */
    bool (*port_status)(uint8_t port, bool *connected, enum usb_speed *speed);

    /* Drive the port reset sequence (connected → DEFAULT). */
    int  (*port_reset)(uint8_t port);

    /*
     * Create HCD-side state for a newly enumerated device. Called by
     * usb_core once the device address has been assigned. Sets up the
     * XHCI slot context, input context, and endpoint 0 ring.
     */
    int  (*device_open)(struct usb_device *dev);

    /* Tear down HCD-side state for a device that's going away. */
    void (*device_close)(struct usb_device *dev);

    /*
     * Configure a non-control endpoint after SET_CONFIGURATION has
     * assigned its context. For XHCI this maps to the
     * CONFIGURE_ENDPOINT command; for simpler HCDs it may be a no-op.
     */
    int  (*endpoint_configure)(struct usb_device *dev,
                               const struct usb_endpoint *ep);

    /*
     * Submit a URB. Non-blocking: returns 0 on success and calls
     * urb->complete asynchronously (possibly from IRQ context).
     */
    int  (*submit_urb)(struct usb_urb *urb);

    /* Best-effort cancellation. Completion still fires with CANCELLED. */
    int  (*cancel_urb)(struct usb_urb *urb);

    /*
     * Drive completions in polled mode — controller drivers that rely
     * on IRQs may leave this NULL. Called from usb_core_poll() so the
     * shell/tests can run without IRQs online.
     */
    void (*poll)(void);
};

/* -------------------------------------------------------------------------- */
/* Core API                                                                    */
/* -------------------------------------------------------------------------- */

/* Register the single active HCD. Must be called before usb_core_start(). */
void usb_core_register_hcd(const struct usb_hcd *hcd);
const struct usb_hcd *usb_core_get_hcd(void);

/*
 * Start the registered HCD and run root-port enumeration for whatever
 * device is connected at boot. On success a single struct usb_device
 * is available via usb_core_first_device(); NULL if no device.
 */
int usb_core_start(void);

/* Drive pending HCD work — call from net_poll() or the shell mainloop. */
void usb_core_poll(void);

/*
 * Enumerate the root-port device: assign address, pull descriptors,
 * parse interfaces/endpoints, and set the default configuration. Idempotent
 * once state is USB_STATE_CONFIGURED.
 */
int usb_core_enumerate(void);

struct usb_device *usb_core_first_device(void);

/* -------------------------------------------------------------------------- */
/* Transfer helpers                                                            */
/* -------------------------------------------------------------------------- */

/*
 * Submit a URB and, if complete is NULL, spin-poll until it finishes or
 * the timeout expires. Uses CNTPCT-based timeouts on ARM64. Returns
 * the URB's final status. Safe to call from task context; do NOT call
 * from IRQ context when complete is NULL (blocking).
 */
int usb_submit_urb(struct usb_urb *urb);
int usb_cancel_urb(struct usb_urb *urb);

/*
 * Blocking control transfer helper — mirrors Linux's usb_control_msg().
 * Builds a SETUP packet, submits, and waits. Returns bytes transferred
 * on success or a negative usb_urb_status code on error.
 */
int usb_control_msg(struct usb_device *dev,
                    uint8_t  bmRequestType,
                    uint8_t  bRequest,
                    uint16_t wValue,
                    uint16_t wIndex,
                    void    *data,
                    uint16_t wLength,
                    uint32_t timeout_ms);

/* Convenience wrapper: GET_DESCRIPTOR (standard, device recipient). */
int usb_get_descriptor(struct usb_device *dev,
                       uint8_t  desc_type,
                       uint8_t  desc_index,
                       void    *buf,
                       uint16_t length);

/* Walk raw_config and populate ifaces[]/endpoints[]. */
int usb_parse_configuration(struct usb_device *dev);

/* Look up the first endpoint on an interface matching (direction, xfer_type). */
const struct usb_endpoint *
usb_find_endpoint(const struct usb_device *dev,
                  uint8_t interface_number,
                  uint8_t direction,
                  uint8_t xfer_type);

/* Return a human-readable name for a urb status code (never NULL). */
const char *usb_urb_status_str(enum usb_urb_status s);

#endif /* USB_H */
