// W5500 bring-up for the Waveshare ESP32-S3-ETH.
//
// The upstream ESPEthernetDriver::Init() targets the ESP32 internal EMAC, which
// the S3 does not have. main/CMakeLists.txt excludes that implementation and this
// file provides the replacement. CHIP calls it during esp_matter::start().
//
// The MLD handling (NETIF_FLAG_MLD6 and joining ff02::1) is carried over from
// matter-esp32-heating-monitor: without it an MLD-snooping switch prunes router
// advertisements and mDNS on our port, and commissioning stops working after a
// while.

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/mld6.h"
#include "lwip/netif.h"

#include <platform/ESP32/NetworkCommissioningDriver.h>

#define ETH_SPI_HOST SPI2_HOST
#define ETH_SPI_SCLK_GPIO 13
#define ETH_SPI_MOSI_GPIO 11
#define ETH_SPI_MISO_GPIO 12
#define ETH_SPI_CS_GPIO 14
#define ETH_SPI_INT_GPIO 10
#define ETH_SPI_RST_GPIO 9
#define ETH_SPI_CLOCK_MHZ 25

static const char *TAG = "matter_ethernet_driver";

namespace chip {
namespace DeviceLayer {
namespace NetworkCommissioning {

// Tracks whether we hold a membership on ff02::1, so a link bounce re-joins
// exactly once rather than stacking up lwIP group use counts.
static bool s_joined_all_nodes = false;

// mld6_joingroup_netif() touches the group list, so it has to run in the TCPIP context.
static esp_err_t join_all_nodes_cb(void *ctx)
{
    struct netif *lwip_netif = (struct netif *)ctx;
    ip6_addr_t allnodes;
    IP6_ADDR(&allnodes, PP_HTONL(0xff020000), PP_HTONL(0x00000000), PP_HTONL(0x00000000), PP_HTONL(0x00000001));
    ip6_addr_assign_zone(&allnodes, IP6_MULTICAST, lwip_netif);
    return (mld6_joingroup_netif(lwip_netif, &allnodes) == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void on_ip6_event(void *esp_netif, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
    // GOT_IP6 fires once per address and for every netif; only act once, for ours.
    if (event == nullptr || event->esp_netif != (esp_netif_t *)esp_netif || s_joined_all_nodes) {
        return;
    }

    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(event->esp_netif);
    if (lwip_netif == nullptr) {
        return;
    }

    esp_err_t err = esp_netif_tcpip_exec(join_all_nodes_cb, lwip_netif);
    s_joined_all_nodes = (err == ESP_OK);
    ESP_LOGI(TAG, "MLD join ff02::1: %s", esp_err_to_name(err));
}

static void on_eth_event(void *esp_netif, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_netif_t *eth_netif = static_cast<esp_netif_t *>(esp_netif);

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Ethernet link up");
        // lwIP only joins solicited-node groups on interfaces flagged MLD6, and
        // esp_netif does not set the flag for Ethernet.
        struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(eth_netif);
        if (lwip_netif != nullptr) {
            netif_set_flags(lwip_netif, NETIF_FLAG_MLD6);
        }
        ESP_ERROR_CHECK(esp_netif_create_ip6_linklocal(eth_netif));
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet link down");
        // The netif drops its group memberships with the link.
        s_joined_all_nodes = false;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

CHIP_ERROR ESPEthernetDriver::Init(NetworkStatusChangeCallback *networkStatusChangeCallback)
{
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = ETH_SPI_MOSI_GPIO;
    buscfg.miso_io_num = ETH_SPI_MISO_GPIO;
    buscfg.sclk_io_num = ETH_SPI_SCLK_GPIO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_devcfg = {};
    spi_devcfg.mode = 0;
    spi_devcfg.clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000;
    spi_devcfg.spics_io_num = ETH_SPI_CS_GPIO;
    spi_devcfg.queue_size = 20;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = ETH_SPI_INT_GPIO;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = ETH_SPI_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));

    // The W5500 has no burned-in MAC; use the ESP32's Ethernet MAC from eFuse.
    uint8_t mac_addr[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, eth_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &on_ip6_event, eth_netif));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "W5500 Ethernet initialized for Matter");
    return CHIP_NO_ERROR;
}

} // namespace NetworkCommissioning
} // namespace DeviceLayer
} // namespace chip
