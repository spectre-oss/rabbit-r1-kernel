// SPDX-License-Identifier: GPL-2.0
/* Standard Linux TCPM/TCPCI for the powered dock's USB-PD data-role swap.
 * Sink-only power policy: this module can never enable the R1 boost output.
 * No automatic MUSB role connection; userspace owns explicit data choices. */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/mutex.h>
#include <linux/usb/tcpci.h>
#include <linux/usb/tcpm.h>
#include <linux/usb/pd.h>
static struct i2c_client *client;
static struct tcpci *port;
static struct i2c_adapter *timing_bus;
static int saved_udelay;
static unsigned int bus_delay=5;
module_param(bus_delay,uint,0400);
static void restore_timing(void)
{
 if(!timing_bus)return;
 i2c_lock_bus(timing_bus,I2C_LOCK_ROOT_ADAPTER);
 ((struct i2c_algo_bit_data *)timing_bus->algo_data)->udelay=saved_udelay;
 i2c_unlock_bus(timing_bus,I2C_LOCK_ROOT_ADAPTER);
 i2c_put_adapter(timing_bus);timing_bus=NULL;
}
static const u32 sink_pdos[]={PDO_FIXED(5000,500,PDO_FIXED_USB_COMM|PDO_FIXED_DATA_SWAP)};
static const struct property_entry props[]={
 PROPERTY_ENTRY_STRING("power-role","sink"),
 PROPERTY_ENTRY_STRING("data-role","dual"),
 PROPERTY_ENTRY_U32_ARRAY("sink-pdos",sink_pdos),
 PROPERTY_ENTRY_U32("op-sink-microwatt",2500000),{}
};
static const struct software_node node={.name="r1-tcpc"};
static const struct software_node connector={.name="connector",.parent=&node,.properties=props};
static const struct software_node *nodes[]={&node,&connector,NULL};
static const struct regmap_config cfg={.reg_bits=8,.val_bits=8,.max_register=0xff};
static int init(struct tcpci *tc,struct tcpci_data *data)
{
 static const struct reg_sequence seq[]={
  REG_SEQ(0x81,0x38,0),REG_SEQ(0x82,0x82,0),REG_SEQ(0xba,0xfc,0),REG_SEQ(0xbb,0x50,0),
  REG_SEQ(0x9e,0x8f,0),REG_SEQ(0xa1,5,0),REG_SEQ(0xa2,4,0),REG_SEQ(0xa3,0x4a,0),
  REG_SEQ(0xa4,1,0),REG_SEQ(0x95,1,0),REG_SEQ(0x80,0x71,0),REG_SEQ(0x9b,0x32,1000),
 };
 return regmap_register_patch(data->regmap,seq,ARRAY_SIZE(seq));
}
static int vbus(struct tcpci *tc,struct tcpci_data *data,bool source,bool sink)
{ return source ? -EPERM : 0; }
static struct tcpci_data data={.init=init,.set_vbus=vbus};
/* Stock DTBO routes the active-low TCPC alert to GPIO41, not IDDIG.
 * Serialize the threaded IRQ and slow recovery check: tcpci_irq consumes RX
 * messages and must never run concurrently with itself. */
static struct gpio_device *alert_gdev;
static struct gpio_desc *alert_gpio;
static int alert_irq;
static DEFINE_MUTEX(alert_lock);
static unsigned long irq_events, recovery_checks;
module_param(irq_events, ulong, 0444);
module_param(recovery_checks, ulong, 0444);
static irqreturn_t alert_thread(int irq, void *dev_id)
{
 irqreturn_t ret;
 mutex_lock(&alert_lock);
 irq_events++;
 ret=tcpci_irq(port);
 mutex_unlock(&alert_lock);
 return ret;
}
static void poll(struct work_struct *work);
static DECLARE_DELAYED_WORK(poller,poll);
static void poll(struct work_struct *work)
{
 mutex_lock(&alert_lock);
 recovery_checks++;
 tcpci_irq(port);
 /* An already-connected cable can lose its first CC edge in TCPM reset.
  * Retain the proven real-CC refresh, but no millisecond bus polling. */
 tcpm_cc_change(tcpci_get_tcpm_port(port));
 mutex_unlock(&alert_lock);
 schedule_delayed_work(&poller,msecs_to_jiffies(1000));
}
static void free_alert_gpio(void)
{
 if (!IS_ERR_OR_NULL(alert_gpio)) gpiochip_free_own_desc(alert_gpio);
 if (alert_gdev) gpio_device_put(alert_gdev);
}
static int __init start(void)
{
 struct i2c_adapter *bus=NULL;int i,ret;unsigned int id;
 struct i2c_board_info info={I2C_BOARD_INFO("r1-tcpc",0x4e),.swnode=&node};
 if(bus_delay<2 || bus_delay>10)return -EINVAL;
 ret=software_node_register_node_group(nodes);if(ret)return ret;
 for(i=0;i<32;i++) {
  bus=i2c_get_adapter(i);
  if(!bus)continue;
  if(!strcmp(bus->name,"R1 Type-C GPIO I2C"))break;
  i2c_put_adapter(bus);
  bus=NULL;
 }
 if(!bus){ret=-ENODEV;goto nodes_out;}
 timing_bus=bus;
 i2c_lock_bus(bus,I2C_LOCK_ROOT_ADAPTER);
 saved_udelay=((struct i2c_algo_bit_data *)bus->algo_data)->udelay;
 ((struct i2c_algo_bit_data *)bus->algo_data)->udelay=bus_delay;
 i2c_unlock_bus(bus,I2C_LOCK_ROOT_ADAPTER);
 client=i2c_new_client_device(bus,&info);
 if(IS_ERR(client)){ret=PTR_ERR(client);goto nodes_out;}
 data.regmap=devm_regmap_init_i2c(client,&cfg);
 if(IS_ERR(data.regmap)){ret=PTR_ERR(data.regmap);goto client_out;}
 ret=regmap_read(data.regmap,0,&id);if(ret||id!=0xcf){ret=ret?ret:-ENODEV;goto client_out;}
 alert_gdev=gpio_device_find_by_label("pinctrl_paris");
 if(!alert_gdev){ret=-ENODEV;goto client_out;}
 alert_gpio=gpiochip_request_own_desc(gpio_device_get_chip(alert_gdev),41,
                                    "r1-tcpc-alert",GPIO_ACTIVE_HIGH,GPIOD_IN);
 if(IS_ERR(alert_gpio)){ret=PTR_ERR(alert_gpio);goto gpio_out;}
 alert_irq=gpiod_to_irq(alert_gpio);
 if(alert_irq<0){ret=alert_irq;goto gpio_out;}
 port=tcpci_register_port(&client->dev,&data);
 if(IS_ERR(port)){ret=PTR_ERR(port);goto gpio_out;}
 ret=request_threaded_irq(alert_irq,NULL,alert_thread,
                          IRQF_TRIGGER_LOW|IRQF_ONESHOT,"r1-tcpc-alert",port);
 if(ret)goto port_out;
 schedule_delayed_work(&poller,msecs_to_jiffies(1000));
 pr_info("r1_usb_tcpc: GPIO41 IRQ %d, 1000ms recovery, bus delay %u us\n",
         alert_irq,bus_delay);
 return 0;
port_out:tcpci_unregister_port(port);
gpio_out:free_alert_gpio();
client_out:i2c_unregister_device(client);
nodes_out:restore_timing();software_node_unregister_node_group(nodes);return ret;
}
static void __exit stop(void)
{
 free_irq(alert_irq,port);
 cancel_delayed_work_sync(&poller);
 tcpci_unregister_port(port);
 free_alert_gpio();
 i2c_unregister_device(client);
 restore_timing();
 software_node_unregister_node_group(nodes);
}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("R1 powered dock Type-C PD data-role control");
