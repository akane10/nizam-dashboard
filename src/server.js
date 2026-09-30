const fs = require("node:fs");
const path = require("node:path");
const express = require("express");
const { engine } = require("express-handlebars");
const units = require("./units");

function loadEnv() {
  const envPath = path.join(__dirname, "..", ".env");
  let text;
  try {
    text = fs.readFileSync(envPath, "utf8");
  } catch {
    return;
  }

  for (const line of text.split("\n")) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith("#")) {
      continue;
    }
    const index = trimmed.indexOf("=");
    if (index === -1) {
      continue;
    }
    const key = trimmed.slice(0, index).trim();
    let value = trimmed.slice(index + 1).trim();
    if (
      (value.startsWith('"') && value.endsWith('"')) ||
      (value.startsWith("'") && value.endsWith("'"))
    ) {
      value = value.slice(1, -1);
    }
    if (key && process.env[key] === undefined) {
      process.env[key] = value;
    }
  }
}

loadEnv();

const app = express();
const PORT = Number(process.env.PORT) || 3000;
const API_KEY = process.env.API_KEY || "";

app.engine(
  "handlebars",
  engine({
    defaultLayout: "main",
    helpers: {
      statusLabel(status) {
        if (status === "trip") {
          return "Trip";
        }
        if (status === "normal") {
          return "Normal";
        }
        return "Offline";
      },
      statusClass(status) {
        return `is-${status}`;
      },
      unitNormalCount(transporters) {
        return transporters.filter((item) => item.status === "normal").length;
      },
    },
  }),
);
app.set("view engine", "handlebars");
app.set("views", path.join(__dirname, "..", "views"));

const logFile = path.join(__dirname, "..", "logs", "dashboard.log");
fs.mkdirSync(path.dirname(logFile), { recursive: true });

function log(message) {
  const line = `${new Date().toISOString()} ${message}`;
  console.log(line);
  fs.appendFile(logFile, `${line}\n`, () => {});
}

app.use((req, res, next) => {
  res.on("finish", () => {
    const detail = res.locals.logDetail ? ` ${res.locals.logDetail}` : "";
    log(`${req.ip} ${req.method} ${req.originalUrl} ${res.statusCode}${detail}`);
  });
  next();
});

app.use(express.json());
app.use(express.static(path.join(__dirname, "..", "public")));

function requireApiKey(req, res, next) {
  if (!API_KEY) {
    return next();
  }

  if (req.get("X-API-KEY") !== API_KEY) {
    return res.status(401).json({ error: "Invalid API key" });
  }

  return next();
}

app.get("/", (req, res) => {
  res.render("dashboard", {
    title: "BUMER - Transporter Monitoring System",
    units: units.listUnits(),
    summary: units.summary(),
  });
});

app.get("/api/units", (req, res) => {
  const summary = units.summary();
  res.locals.logDetail = `normal=${summary.normal} trip=${summary.trip} offline=${summary.offline}`;
  res.json({
    units: units.listUnits(),
    summary,
  });
});

app.post("/api/status", requireApiKey, (req, res) => {
  const { unit, transporter, status, counter } = req.body ?? {};
  const result = units.applyStatus(unit, transporter, status, counter);

  if (result.error) {
    const code = result.error.includes("not found") ? 404 : 400;
    res.locals.logDetail = result.error;
    return res.status(code).json({ error: result.error });
  }

  res.locals.logDetail = `unit ${unit} ${transporter} ${status} -> ${result.transporter.status} counter=${result.transporter.counter}`;
  res.json({
    transporter: result.transporter,
    units: units.listUnits(),
    summary: units.summary(),
  });
});

app.post("/api/reset", (req, res) => {
  const { unit, transporter } = req.body ?? {};
  const result = units.queueReset(unit, transporter);

  if (result.error) {
    const code = result.error.includes("not found") ? 404 : 400;
    res.locals.logDetail = result.error;
    return res.status(code).json({ error: result.error });
  }

  res.locals.logDetail = `queued ${result.pending}`;
  res.json({
    pending: result.pending,
    units: units.listUnits(),
    summary: units.summary(),
  });
});

app.get("/api/reset-check/:unitId", requireApiKey, (req, res) => {
  const labels = units.takePendingResets(req.params.unitId);
  if (!labels) {
    res.locals.logDetail = "unit not found";
    return res.status(404).type("text/plain").send("Unit not found");
  }

  res.locals.logDetail = labels.length ? labels.join(",") : "none";
  res.type("text/plain").send(labels.join(","));
});

app.post("/api/heartbeat", requireApiKey, (req, res) => {
  const unit = units.heartbeat(req.body?.unit);
  if (!unit) {
    res.locals.logDetail = "unit not found";
    return res.status(404).json({ error: "Unit not found" });
  }

  const summary = units.summary();
  res.locals.logDetail = `unit ${unit.id} online=${unit.online} normal=${summary.normal} trip=${summary.trip} offline=${summary.offline}`;
  res.json({
    unit,
    units: units.listUnits(),
    summary,
  });
});

app.listen(PORT, () => {
  log(`Dashboard running at http://localhost:${PORT}`);
});
