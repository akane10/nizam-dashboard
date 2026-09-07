const relativeTime = new Intl.RelativeTimeFormat("en", { numeric: "auto" });

function formatUpdated(isoString) {
  const then = new Date(isoString).getTime();
  const deltaSeconds = Math.round((then - Date.now()) / 1000);
  const abs = Math.abs(deltaSeconds);

  if (abs < 60) {
    return relativeTime.format(deltaSeconds, "second");
  }
  if (abs < 3600) {
    return relativeTime.format(Math.round(deltaSeconds / 60), "minute");
  }
  return relativeTime.format(Math.round(deltaSeconds / 3600), "hour");
}

function statusLabel(status) {
  if (status === "trip") {
    return "Trip";
  }
  if (status === "normal") {
    return "Normal";
  }
  return "Offline";
}

function render(payload) {
  document.querySelector("[data-units]").textContent = payload.summary.units;
  document.querySelector("[data-normal]").textContent = payload.summary.normal;
  document.querySelector("[data-trip]").textContent = payload.summary.trip;
  document.querySelector("[data-offline]").textContent = payload.summary.offline;

  for (const unit of payload.units) {
    const unitCard = document.querySelector(`[data-unit-id="${unit.id}"]`);
    if (!unitCard) {
      continue;
    }

    const normalCount = unit.transporters.filter((item) => item.status === "normal").length;
    unitCard.querySelector("[data-unit-normal]").textContent = normalCount;

    for (const transporter of unit.transporters) {
      const card = unitCard.querySelector(`[data-transporter-id="${transporter.id}"]`);
      if (!card) {
        continue;
      }

      card.classList.remove("is-normal", "is-trip", "is-offline");
      card.classList.add(`is-${transporter.status}`);
      card.querySelector("[data-status-label]").textContent = statusLabel(transporter.status);

      const time = card.querySelector("[data-updated]");
      time.dateTime = transporter.lastChange;
      time.textContent = formatUpdated(transporter.lastChange);
    }
  }
}

async function loadUnits() {
  const response = await fetch("/api/units");
  render(await response.json());
}

loadUnits();
setInterval(loadUnits, 5000);
