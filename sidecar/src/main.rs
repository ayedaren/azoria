use ddc_hi::{Ddc, Display};
use hidapi::{HidApi, HidDevice};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::env;
use std::process::ExitCode;
use std::time::{Duration, Instant};

const LG_VENDOR_ID: u16 = 0x043e;
const LG_PRODUCT_ID: u16 = 0x9a39;
const REPORT_SIZE: usize = 64;
const DDC_DESTINATION: u8 = 0x6e;

#[derive(Debug, Deserialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
enum Request {
    Probe {
        transport: Transport,
    },
    Enumerate,
    Get {
        transport: Transport,
        display: usize,
        vcp: u8,
    },
    Set {
        transport: Transport,
        display: usize,
        vcp: u8,
        value: u16,
    },
    Input {
        transport: Transport,
        source: InputSource,
    },
}

#[derive(Clone, Copy, Debug, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum Transport {
    NativeDdc,
    LgHidDdc,
}

#[derive(Clone, Copy, Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
enum InputSource {
    Dp1,
    Dp2,
    Hdmi1,
    Hdmi2,
    Usbc,
}

#[derive(Debug, Serialize)]
struct Response {
    ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    result: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

impl Response {
    fn success(result: Value) -> Self {
        Self {
            ok: true,
            result: Some(result),
            error: None,
        }
    }

    fn failure(error: impl Into<String>) -> Self {
        Self {
            ok: false,
            result: None,
            error: Some(error.into()),
        }
    }
}

fn native_displays() -> Vec<Display> {
    Display::enumerate()
}

fn native_display(display: usize) -> Result<Display, String> {
    if display == 0 {
        return Err("display indexes start at 1".into());
    }
    native_displays()
        .into_iter()
        .nth(display - 1)
        .ok_or_else(|| format!("display {display} was not found"))
}

fn native_get(display: usize, vcp: u8) -> Result<Value, String> {
    let mut display = native_display(display)?;
    let value = display
        .handle
        .get_vcp_feature(vcp)
        .map_err(|error| format!("DDC/CI Get VCP 0x{vcp:02x} failed: {error}"))?;
    Ok(json!({
        "driver": display.info.backend.to_string(),
        "id": display.info.id,
        "vcp": vcp,
        "maximum": value.maximum(),
        "current": value.value()
    }))
}

fn native_set(display: usize, vcp: u8, value: u16) -> Result<Value, String> {
    let mut display = native_display(display)?;
    display
        .handle
        .set_vcp_feature(vcp, value)
        .map_err(|error| format!("DDC/CI Set VCP 0x{vcp:02x} failed: {error}"))?;
    Ok(json!({ "vcp": vcp, "value": value, "acknowledged": true }))
}

fn lg_device() -> Result<HidDevice, String> {
    HidApi::new()
        .map_err(|error| format!("HID initialization failed: {error}"))?
        .open(LG_VENDOR_ID, LG_PRODUCT_ID)
        .map_err(|error| format!("LG Monitor Controls HID 043e:9a39 is not connected: {error}"))
}

fn hid_write(device: &HidDevice, report: &[u8; REPORT_SIZE]) -> Result<(), String> {
    let mut wire = [0_u8; REPORT_SIZE + 1];
    wire[1..].copy_from_slice(report);
    device
        .write(&wire)
        .map_err(|error| format!("LG HID write failed: {error}"))?;
    Ok(())
}

fn response_payload<'a>(buffer: &'a [u8], length: usize) -> &'a [u8] {
    let payload = &buffer[..length.min(buffer.len())];
    if payload.len() > REPORT_SIZE && payload[0] == 0 {
        &payload[1..]
    } else {
        payload
    }
}

fn validate_get_response(report: &[u8], opcode: u8) -> Result<(u16, u16), String> {
    if report.len() < 15 || report[6] != 0x02 {
        return Err("LG HID response is not a DDC/CI Get VCP reply".into());
    }
    if report[7] != 0 {
        return Err(format!(
            "LG HID DDC/CI Get VCP failed with result 0x{:02x}",
            report[7]
        ));
    }
    if report[8] != opcode {
        return Err("LG HID response opcode does not match request".into());
    }
    let ddc_length = usize::from(report[5] & 0x7f);
    let end = 5_usize
        .checked_add(ddc_length)
        .and_then(|value| value.checked_add(2))
        .ok_or_else(|| "LG HID response length overflow".to_string())?;
    if end > report.len() {
        return Err("LG HID response is truncated".into());
    }
    let checksum = report[5..end]
        .iter()
        .fold(DDC_DESTINATION ^ 0x50, |current, byte| current ^ byte);
    if checksum != 0 {
        return Err(format!(
            "LG HID response checksum failed with 0x{checksum:02x}"
        ));
    }
    Ok((
        u16::from_be_bytes([report[10], report[11]]),
        u16::from_be_bytes([report[12], report[13]]),
    ))
}

fn lg_exchange(
    source: u8,
    opcode: u8,
    payload: &[u8],
    expected_length: u8,
    response_required: bool,
) -> Result<Option<(u16, u16)>, String> {
    if payload.len() > 52 {
        return Err("invalid HID DDC request".into());
    }
    let device = lg_device()?;
    let mut report = [0_u8; REPORT_SIZE];
    report[..8].copy_from_slice(&[0x08, 0x01, 0x55, 0x03, 0x00, 0x00, 0x03, 0x37]);
    let mut ddc = Vec::with_capacity(payload.len() + 3);
    ddc.push(source);
    ddc.push(0x80 | u8::try_from(payload.len()).map_err(|_| "DDC payload is too long")?);
    ddc.extend_from_slice(payload);
    let checksum = ddc
        .iter()
        .fold(DDC_DESTINATION, |current, byte| current ^ byte);
    ddc.push(checksum);
    report[4] = u8::try_from(ddc.len()).map_err(|_| "DDC frame is too long")?;
    report[8..8 + ddc.len()].copy_from_slice(&ddc);
    hid_write(&device, &report)?;

    report[1] = 0x02;
    report[3] = 0x04;
    report[4] = expected_length;
    report[6] = 0x0b;
    hid_write(&device, &report)?;
    if !response_required {
        return Ok(None);
    }

    let deadline = Instant::now() + Duration::from_millis(650);
    let mut buffer = [0_u8; REPORT_SIZE + 1];
    while Instant::now() < deadline {
        let remaining = deadline.saturating_duration_since(Instant::now());
        let timeout = i32::try_from(remaining.as_millis().max(1)).unwrap_or(650);
        let length = device
            .read_timeout(&mut buffer, timeout)
            .map_err(|error| format!("LG HID read failed: {error}"))?;
        if length == 0 {
            continue;
        }
        let response = response_payload(&buffer, length);
        if response.len() >= 9 && response[8] == opcode {
            return validate_get_response(response, opcode).map(Some);
        }
    }
    Err("LG HID response timed out".into())
}

fn lg_get(vcp: u8) -> Result<Value, String> {
    let payload = [0x01, vcp];
    let (maximum, current) = lg_exchange(0x51, vcp, &payload, 0x0b, true)?
        .ok_or_else(|| "LG HID did not return a value".to_string())?;
    Ok(json!({ "vcp": vcp, "maximum": maximum, "current": current }))
}

fn lg_set(vcp: u8, value: u16) -> Result<Value, String> {
    let [high, low] = value.to_be_bytes();
    let payload = [0x03, vcp, high, low];
    lg_exchange(0x51, vcp, &payload, 0x0b, false)?;
    Ok(json!({ "vcp": vcp, "value": value, "acknowledged": true }))
}

fn lg_input(source: InputSource) -> Result<Value, String> {
    let value: u16 = match source {
        InputSource::Dp1 => 0xd0,
        InputSource::Dp2 => 0xd1,
        InputSource::Hdmi1 => 0x90,
        InputSource::Hdmi2 => 0x91,
        InputSource::Usbc => 0xd2,
    };
    let [high, low] = value.to_be_bytes();
    let payload = [0x03, 0xf4, high, low];
    lg_exchange(0x50, 0xf4, &payload, 0x26, false)?;
    Ok(json!({ "vcp": 0xf4, "value": value, "acknowledged": true }))
}

fn execute(request: Request) -> Result<Value, String> {
    match request {
        Request::Probe {
            transport: Transport::NativeDdc,
        } => {
            let displays = native_displays();
            Ok(json!({ "transport": "native-ddc", "count": displays.len() }))
        }
        Request::Probe {
            transport: Transport::LgHidDdc,
        } => {
            lg_device()?;
            Ok(
                json!({ "transport": "lg-hid-ddc", "vendor_id": LG_VENDOR_ID, "product_id": LG_PRODUCT_ID }),
            )
        }
        Request::Enumerate => Ok(Value::Array(
            native_displays()
                .into_iter()
                .enumerate()
                .map(|(index, display)| {
                    json!({
                        "display": index + 1,
                        "driver": display.info.backend.to_string(),
                        "id": display.info.id,
                        "manufacturer": display.info.manufacturer_id,
                        "model": display.info.model_name
                    })
                })
                .collect(),
        )),
        Request::Get {
            transport: Transport::NativeDdc,
            display,
            vcp,
        } => native_get(display, vcp),
        Request::Get {
            transport: Transport::LgHidDdc,
            vcp,
            ..
        } => lg_get(vcp),
        Request::Set {
            transport: Transport::NativeDdc,
            display,
            vcp,
            value,
        } => native_set(display, vcp, value),
        Request::Set {
            transport: Transport::LgHidDdc,
            vcp,
            value,
            ..
        } => lg_set(vcp, value),
        Request::Input {
            transport: Transport::LgHidDdc,
            source,
        } => lg_input(source),
        Request::Input {
            transport: Transport::NativeDdc,
            ..
        } => Err("native input changes use Set VCP 0x60".into()),
    }
}

fn main() -> ExitCode {
    let request = match env::args().nth(1) {
        Some(raw) if raw.len() <= 2048 => serde_json::from_str::<Request>(&raw)
            .map_err(|error| format!("invalid request: {error}")),
        Some(_) => Err("request is too large".into()),
        None => Err("one JSON request argument is required".into()),
    };
    let response = match request.and_then(execute) {
        Ok(result) => Response::success(result),
        Err(error) => Response::failure(error),
    };
    println!(
        "{}",
        serde_json::to_string(&response).expect("response serialization cannot fail")
    );
    if response.ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::from(1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_known_good_lg_reply() {
        let report = [
            0x0f, 0x02, 0x55, 0x00, 0x6e, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x64,
            0xa4,
        ];
        assert_eq!(validate_get_response(&report, 0x10).unwrap(), (100, 100));
    }

    #[test]
    fn rejects_ddc_failure_reply_instead_of_returning_zero() {
        let mut report = [
            0x0f, 0x02, 0x55, 0x00, 0x6e, 0x88, 0x02, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00,
        ];
        report[14] = report[5..14]
            .iter()
            .fold(DDC_DESTINATION ^ 0x50, |current, byte| current ^ byte);
        assert!(validate_get_response(&report, 0x10).is_err());
    }
}
