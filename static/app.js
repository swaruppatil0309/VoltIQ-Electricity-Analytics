let trendChart, forecastChart;
const $ = id => document.getElementById(id);
function fmt(v, digits=3){ return v === null || v === undefined ? "N/A" : Number(v).toFixed(digits); }
function queryString(){
  const p = new URLSearchParams();
  if($("startDate").value) p.set("start",$("startDate").value);
  if($("endDate").value) p.set("end",$("endDate").value);
  return p.toString();
}
async function loadAnalytics(){
  $("message").className="message";
  $("message").textContent="Calculating analytics…";
  const qs=queryString();
  $("downloadBtn").href="/api/export"+(qs?"?"+qs:"");
  try{
    const response=await fetch("/api/analytics"+(qs?"?"+qs:""));
    const data=await response.json();
    if(!response.ok) throw new Error(data.error||"Unable to load data");
    $("message").textContent=`Dataset loaded • ${data.start_date} to ${data.end_date}`;
    $("mean").textContent=fmt(data.mean); $("median").textContent=fmt(data.median);
    $("std").textContent=fmt(data.std); $("count").textContent=Number(data.count).toLocaleString();
    $("prob").textContent=(data.prob_above_mean*100).toFixed(2)+"%";
    $("ciLow").textContent=fmt(data.ci[0]); $("ciHigh").textContent=fmt(data.ci[1]);
    $("outliers").textContent=Number(data.outlier_count).toLocaleString()+" ("+data.outlier_pct+"%)";
    $("min").textContent=fmt(data.min); $("max").textContent=fmt(data.max);
    $("dateRange").textContent=`Date range: ${data.start_date} — ${data.end_date}`;
    $("mae").textContent=fmt(data.regression.mae); $("rmse").textContent=fmt(data.regression.rmse);
    $("r2").textContent=fmt(data.regression.r2);
    drawTrend(data); drawForecast(data);
  }catch(err){ $("message").className="message error"; $("message").textContent=err.message; }
}
function drawTrend(d){
  if(trendChart) trendChart.destroy();
  trendChart=new Chart($("trendChart"),{type:"line",data:{labels:d.dates,datasets:[{label:"Daily mean (kW)",data:d.daily_mean,borderColor:"#35baff",backgroundColor:"#35baff22",fill:true,tension:.28,pointRadius:1}]},options:{responsive:true,plugins:{legend:{labels:{color:"#a9bfd4"}}},scales:{x:{ticks:{color:"#7892aa",maxTicksLimit:10},grid:{color:"#1b3046"}},y:{ticks:{color:"#7892aa"},grid:{color:"#1b3046"}}}}});
}
function drawForecast(d){
  if(forecastChart) forecastChart.destroy();
  const labels=d.dates.slice(-Math.min(20,d.dates.length));
  const values=d.daily_mean.slice(-labels.length);
  const future=d.forecast||[];
  const last=d.dates.length?new Date(d.dates[d.dates.length-1]+"T00:00:00"):new Date();
  const futureLabels=future.map((_,i)=>{let x=new Date(last);x.setDate(x.getDate()+i+1);return x.toISOString().slice(0,10)});
  forecastChart=new Chart($("forecastChart"),{type:"line",data:{labels:[...labels,...futureLabels],datasets:[{label:"Observed daily mean",data:[...values,...Array(future.length).fill(null)],borderColor:"#35baff",tension:.25,pointRadius:2},{label:"7-day forecast",data:[...Array(Math.max(0,values.length-1)).fill(null),values[values.length-1]??null,...future],borderColor:"#53e3c2",borderDash:[6,5],tension:.25,pointRadius:3}]},options:{responsive:true,plugins:{legend:{labels:{color:"#a9bfd4"}}},scales:{x:{ticks:{color:"#7892aa",maxTicksLimit:10},grid:{color:"#1b3046"}},y:{ticks:{color:"#7892aa"},grid:{color:"#1b3046"}}}}});
}
$("applyBtn").addEventListener("click",loadAnalytics);
$("resetBtn").addEventListener("click",()=>{$("startDate").value="";$("endDate").value="";loadAnalytics()});
loadAnalytics();
