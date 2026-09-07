const path = require("node:path");
const express = require("express");
const { engine } = require("express-handlebars");
const units = require("./units");

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
    title: "Unit Dashboard",
    units: units.listUnits(),
    summary: units.summary(),
  });
});

app.get("/api/units", (req, res) => {
  res.json({
    units: units.listUnits(),
    summary: units.summary(),
  });
});

app.post("/api/status", requireApiKey, (req, res) => {
  const { unit, transporter, status, counter } = req.body ?? {};
  const result = units.applyStatus(unit, transporter, status, counter);

  if (result.error) {
    const code = result.error.includes("not found") ? 404 : 400;
    return res.status(code).json({ error: result.error });
  }

  res.json({
    transporter: result.transporter,
    units: units.listUnits(),
    summary: units.summary(),
  });
});

app.post("/api/heartbeat", requireApiKey, (req, res) => {
  const unit = units.heartbeat(req.body?.unit);
  if (!unit) {
    return res.status(404).json({ error: "Unit not found" });
  }

  res.json({
    unit,
    units: units.listUnits(),
    summary: units.summary(),
  });
});

app.listen(PORT, () => {
  console.log(`Dashboard running at http://localhost:${PORT}`);
});
