/**
 * weather_endpoint.gs — Weather Data Endpoint
 * =============================================
 * Fetches a 24-hour weather forecast from Taiwan's Central Weather
 * Administration (CWA) Open Data API and returns a compact CSV string
 * consumed by the Arduino alarm clock over HTTPS.
 *
 * HOW TO DEPLOY:
 *  1. Open https://script.google.com and create a new project.
 *  2. Paste this file's contents into the editor.
 *  3. Set API_KEY and LOCATION_NAME below.
 *  4. Click Deploy → New deployment → Web app.
 *     - Execute as: Me
 *     - Who has access: Anyone
 *  5. Copy the deployment URL.
 *  6. In config.h, set WEATHER_PATH to the path portion of that URL
 *     (everything after "script.google.com").
 *
 * RESPONSE FORMAT (plain text, no headers):
 *   minTemp,maxTemp,maxPrecipProb,weatherDesc,windLevel
 *
 * Example:
 *   18,26,30,Sunny,L3Brez
 *
 * CWA Open Data portal: https://opendata.cwa.gov.tw
 *
 * Author: Sealion100
 * License: CC BY-NC 4.0
 */

// ==========================================
// Configuration
// ==========================================

/** Your CWA Open Data API key. Register at https://opendata.cwa.gov.tw */
const API_KEY = "YOUR_CWA_API_KEY";

/**
 * URL-encoded city name for the forecast location.
 * Default: Hsinchu City (%E6%96%B0%E7%AB%B9%E5%B8%82)
 * Other examples:
 *   Taipei City    → %E8%87%BA%E5%8C%97%E5%B8%82
 *   Taichung City  → %E8%87%BA%E4%B8%AD%E5%B8%82
 *   Tainan City    → %E5%8F%B0%E5%8D%97%E5%B8%82
 *   Kaohsiung City → %E9%AB%98%E9%9B%84%E5%B8%82
 */
const LOCATION_NAME = "%E6%96%B0%E7%AB%B9%E5%B8%82";

/** Weather elements to fetch (do not modify unless extending the script). */
const ELEMENTS = "MinT,MaxT,PoP,Wx,WS";


// ==========================================
// Main Handler
// ==========================================

/**
 * GET handler — called by Apps Script when the web app is accessed.
 * Fetches the CWA forecast and returns a single CSV line.
 *
 * @returns {ContentService.TextOutput} Plain-text CSV or "Error"
 */
function doGet() {
  const url =
    `https://opendata.cwa.gov.tw/api/v1/rest/datastore/F-C0032-001` +
    `?Authorization=${API_KEY}` +
    `&locationName=${LOCATION_NAME}` +
    `&elementName=${ELEMENTS}`;

  try {
    const response = UrlFetchApp.fetch(url);
    const data     = JSON.parse(response.getContentText());
    const elements = data.records.location[0].weatherElement;

    // ------------------------------------------------------------------
    // Parse the first two forecast periods (0–12 h and 12–24 h)
    // ------------------------------------------------------------------

    // Precipitation probability — maximum across both periods
    const pop1      = parseInt(getParam(elements, "PoP", 0));
    const pop2      = parseInt(getParam(elements, "PoP", 1));
    const maxPop24h = Math.max(pop1, pop2);

    // Minimum temperature — lower of both periods
    const minT1   = parseInt(getParam(elements, "MinT", 0));
    const minT2   = parseInt(getParam(elements, "MinT", 1));
    const minT24h = Math.min(minT1, minT2);

    // Maximum temperature — higher of both periods
    const maxT1   = parseInt(getParam(elements, "MaxT", 0));
    const maxT2   = parseInt(getParam(elements, "MaxT", 1));
    const maxT24h = Math.max(maxT1, maxT2);

    // Weather description & wind level — nearest period only
    const wx = getParam(elements, "Wx", 0);
    const ws = getParam(elements, "WS", 0);

    // ------------------------------------------------------------------
    // Translate weather description to English
    // (CWA returns Traditional Chinese; Arduino cannot handle UTF-8 well)
    // ------------------------------------------------------------------
    const wxEng   = translateWeather(wx);
    const wsClean = ws.replace("級", "L").replace("風", "Brez");

    // Return: minTemp,maxTemp,precipProb,weatherDesc,windLevel
    return ContentService.createTextOutput(
      `${minT24h},${maxT24h},${maxPop24h},${wxEng},${wsClean}`
    );

  } catch (err) {
    Logger.log("Weather fetch error: " + err.message);
    return ContentService.createTextOutput("Error");
  }
}


// ==========================================
// Helper Functions
// ==========================================

/**
 * Retrieve the parameterName for a given weather element and time index.
 *
 * @param {Array}  elements  - The weatherElement array from the CWA API response
 * @param {string} name      - Element name (e.g. "MinT", "PoP")
 * @param {number} timeIndex - Time period index (0 = 0–12 h, 1 = 12–24 h)
 * @returns {string} The parameter value as a string
 */
function getParam(elements, name, timeIndex) {
  return elements
    .find(e => e.elementName === name)
    .time[timeIndex]
    .parameter
    .parameterName;
}

/**
 * Translate a CWA Traditional Chinese weather description to English.
 * Uses substring matching for robustness against compound descriptions.
 *
 * @param {string} wx - Raw weather description from CWA API
 * @returns {string}  English weather label
 */
function translateWeather(wx) {
  if (wx.includes("晴")) return "Sunny";
  if (wx.includes("雨")) return "Rainy";
  if (wx.includes("雷")) return "Storm";
  return "Cloudy";  // Default fallback
}
