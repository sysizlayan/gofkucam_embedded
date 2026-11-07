# SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
import json
from urllib.request import ProxyHandler
from urllib.request import build_opener

import pytest
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.wifi
@idf_parametrize('target', ['esp32c3'], indirect=['target'])
def test_gate_mqtt_tcp_startup(dut):  # type: ignore
    """Read-only checks after normal firmware startup; do not send gate commands."""
    dut.expect('Gate commanded open at', timeout=30)
    ip = dut.expect(r'IPv4 address: (\d+\.\d+\.\d+\.\d+)', timeout=120)[1].decode()
    dut.expect('Subscribed to CatStatus', timeout=60)
    dut.expect('MQTT_EVENT_SUBSCRIBED', timeout=30)
    opener = build_opener(ProxyHandler({}))
    with opener.open(f'http://{ip}/gate_status', timeout=10) as response:
        status = json.load(response)
    assert status['mqtt_connected']
    assert status['state'] in ('open', 'closed', 'recovering')
    assert 'commanded_position' in status
