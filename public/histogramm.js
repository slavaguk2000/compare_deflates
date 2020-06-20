google.charts.load("current", {packages:["corechart"]});
google.charts.setOnLoadCallback(drawChart);

function createPair(str, num) {
    str += '('
    str += num.toFixed(2)
    str += ')'
    return [str, num]
}

function drawChart() {
    times = get_time_of_decompress()
    var data = google.visualization.arrayToDataTable([
        ['Compressor', 'Time'],
        createPair('pako' , times[0]),
        createPair('wasm-flate', times[1]),
        createPair('d5ly', times[2])
    ]);
    var options = {
        title: 'Compress time',
        hAxis: {title: 'Compressor'},
        vAxis: {title: 'Milliseconds'}
    };
    var chart = new google.visualization.ColumnChart(document.getElementById('deflate'));
    chart.draw(data, options);
}