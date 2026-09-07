const OFFLINE_AFTER_MS = 45_000;

const now = () => new Date().toISOString();

function createTransporters() {
  return [1, 2, 3, 4].map((number) => ({
    id: number,
    name: `Transporter no${number}`,
    label: `No.${number}`,
    status: "offline",
    counter: 0,
    lastChange: now(),
  }));
}

const units = [1, 2, 3].map((number) => ({
  id: number,
  name: `Unit ${number}`,
  lastSeen: null,
  transporters: createTransporters(),
}));

function parseTransporterId(value) {
  if (typeof value === "number" && Number.isInteger(value)) {
    return value;
  }

  const text = String(value ?? "");
  const match = text.match(/(\d+)/);
  return match ? Number(match[1]) : NaN;
}

function normalizeStatus(value) {
  const status = String(value ?? "")
    .trim()
    .toLowerCase();

  if (status === "trip") {
    return "trip";
  }
  if (status === "normal") {
    return "normal";
  }
  return null;
}

function getUnit(unitId) {
  return units.find((item) => item.id === Number(unitId));
}

function getTransporter(unitId, transporterId) {
  const unit = getUnit(unitId);
  if (!unit) {
    return null;
  }

  return unit.transporters.find((item) => item.id === Number(transporterId)) ?? null;
}

function isUnitOnline(unit, at = Date.now()) {
  if (!unit.lastSeen) {
    return false;
  }
  return at - Date.parse(unit.lastSeen) <= OFFLINE_AFTER_MS;
}

function displayedStatus(unit, transporter, at = Date.now()) {
  if (!isUnitOnline(unit, at)) {
    return "offline";
  }
  return transporter.status === "trip" ? "trip" : "normal";
}

function decorate(unit, at = Date.now()) {
  const online = isUnitOnline(unit, at);
  return {
    ...unit,
    online,
    transporters: unit.transporters.map((transporter) => ({
      ...transporter,
      status: displayedStatus(unit, transporter, at),
    })),
  };
}

function listUnits() {
  const at = Date.now();
  return units.map((unit) => decorate(unit, at));
}

function touchUnit(unit) {
  unit.lastSeen = now();
}

function applyStatus(unitId, transporterRef, statusValue, counter) {
  const status = normalizeStatus(statusValue);
  if (!status) {
    return { error: "status must be TRIP or NORMAL" };
  }

  const unit = getUnit(unitId);
  if (!unit) {
    return { error: "Unit not found" };
  }

  const transporter = getTransporter(unitId, parseTransporterId(transporterRef));
  if (!transporter) {
    return { error: "Transporter not found" };
  }

  transporter.status = status;
  transporter.lastChange = now();
  if (Number.isFinite(Number(counter))) {
    transporter.counter = Number(counter);
  }
  touchUnit(unit);

  return { transporter: { ...transporter }, unit: decorate(unit) };
}

function heartbeat(unitId) {
  const unit = getUnit(unitId);
  if (!unit) {
    return null;
  }

  touchUnit(unit);
  return decorate(unit);
}

function summary() {
  const all = listUnits().flatMap((unit) => unit.transporters);
  return {
    units: units.length,
    total: all.length,
    normal: all.filter((item) => item.status === "normal").length,
    trip: all.filter((item) => item.status === "trip").length,
    offline: all.filter((item) => item.status === "offline").length,
  };
}

module.exports = {
  listUnits,
  applyStatus,
  heartbeat,
  summary,
};
