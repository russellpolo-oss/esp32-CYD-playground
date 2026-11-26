<?php
declare(strict_types=1);

ini_set('display_errors', '0');
ini_set('log_errors', '1');

const DISPLAY_WIDTH = 320;
const DISPLAY_HEIGHT = 240;
const MAX_SOURCE_BYTES = 25 * 1024 * 1024;
const MAX_SOURCE_PIXELS = 16000000;
const MAX_OUTPUT_BYTES = 96 * 1024;

function sendError(int $status, string $message): void
{
    http_response_code($status);
    header('Content-Type: text/plain; charset=utf-8');
    header('Cache-Control: no-store');
    echo $message;
    exit;
}

function requestOrigin(): string
{
    $forwardedHost = $_SERVER['HTTP_X_FORWARDED_HOST'] ?? '';
    $host = is_string($forwardedHost) && $forwardedHost !== ''
        ? trim(explode(',', $forwardedHost)[0])
        : trim((string)($_SERVER['HTTP_HOST'] ?? $_SERVER['SERVER_NAME'] ?? ''));

    $hostPattern = '/^(?:[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?|\[[0-9A-Fa-f:.]+\])(?::[0-9]{1,5})?$/D';
    if ($host === '' || strlen($host) > 255 || !preg_match($hostPattern, $host)) {
        sendError(400, 'Invalid request host');
    }

    $forwardedProto = $_SERVER['HTTP_X_FORWARDED_PROTO'] ?? '';
    $proto = is_string($forwardedProto)
        ? strtolower(trim(explode(',', $forwardedProto)[0]))
        : '';
    if ($proto !== 'http' && $proto !== 'https') {
        $https = strtolower((string)($_SERVER['HTTPS'] ?? ''));
        $proto = ($https !== '' && $https !== 'off' && $https !== '0') ||
            (int)($_SERVER['SERVER_PORT'] ?? 0) === 443
            ? 'https'
            : 'http';
    }

    $scriptName = str_replace('\\', '/', (string)($_SERVER['SCRIPT_NAME'] ?? '/cydimages.php'));
    if ($scriptName === '' || $scriptName[0] !== '/') {
        $scriptName = '/' . ltrim($scriptName, '/');
    }
    if (preg_match('/[\r\n?#]/', $scriptName)) {
        sendError(400, 'Invalid script path');
    }

    return $proto . '://' . $host . $scriptName;
}

function resolveImagePath(string $filename): ?string
{
    if ($filename === '' || $filename === '.' || $filename === '..' ||
        strpos($filename, '/') !== false || strpos($filename, '\\') !== false) {
        return null;
    }

    $extension = strtolower(pathinfo($filename, PATHINFO_EXTENSION));
    if (!in_array($extension, ['jpg', 'jpeg', 'png'], true)) {
        return null;
    }

    $directory = realpath(__DIR__);
    $path = realpath(__DIR__ . DIRECTORY_SEPARATOR . $filename);
    if ($directory === false || $path === false || !is_file($path) || !is_readable($path)) {
        return null;
    }

    $prefix = rtrim($directory, DIRECTORY_SEPARATOR) . DIRECTORY_SEPARATOR;
    $insideDirectory = DIRECTORY_SEPARATOR === '\\'
        ? strncasecmp($path, $prefix, strlen($prefix)) === 0
        : strncmp($path, $prefix, strlen($prefix)) === 0;
    return $insideDirectory ? $path : null;
}

function listImages(): void
{
    $entries = scandir(__DIR__);
    if ($entries === false) {
        sendError(500, 'Unable to scan image directory');
    }

    $filenames = [];
    foreach ($entries as $entry) {
        if (!preg_match('/\.(?:jpe?g|png)$/i', $entry) ||
            resolveImagePath($entry) === null) {
            continue;
        }
        $filenames[] = $entry;
    }
    sort($filenames, SORT_NATURAL | SORT_FLAG_CASE);

    $baseUrl = requestOrigin();
    $urls = [];
    foreach ($filenames as $filename) {
        $urls[] = $baseUrl . '?image=' . rawurlencode($filename);
    }

    $json = json_encode($urls, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
    if ($json === false) {
        sendError(500, 'Unable to encode image list');
    }

    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    echo $json;
}

function serveImage(string $filename): void
{
    $path = resolveImagePath($filename);
    if ($path === null) {
        sendError(404, 'Image not found');
    }
    if (!function_exists('imagecreatetruecolor')) {
        sendError(500, 'PHP GD extension is required');
    }

    $sourceBytes = filesize($path);
    if ($sourceBytes === false || $sourceBytes > MAX_SOURCE_BYTES) {
        sendError(413, 'Image file is too large');
    }

    $imageInfo = @getimagesize($path);
    if ($imageInfo === false ||
        !in_array($imageInfo[2], [IMAGETYPE_JPEG, IMAGETYPE_PNG], true)) {
        sendError(415, 'Only JPG and PNG images are supported');
    }

    $sourceWidth = (int)$imageInfo[0];
    $sourceHeight = (int)$imageInfo[1];
    if ($sourceWidth < 1 || $sourceHeight < 1 ||
        $sourceWidth * $sourceHeight > MAX_SOURCE_PIXELS) {
        sendError(413, 'Image dimensions are too large');
    }

    if ($imageInfo[2] === IMAGETYPE_JPEG) {
        $source = @imagecreatefromjpeg($path);
    } else {
        $source = @imagecreatefrompng($path);
    }
    if ($source === false) {
        sendError(415, 'Unable to decode image');
    }

    $output = imagecreatetruecolor(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if ($output === false) {
        imagedestroy($source);
        sendError(500, 'Unable to allocate output image');
    }

    $background = imagecolorallocate($output, 0, 0, 0);
    imagefill($output, 0, 0, $background);
    imagealphablending($output, true);

    $scale = max(DISPLAY_WIDTH / $sourceWidth, DISPLAY_HEIGHT / $sourceHeight);
    $cropWidth = (int)round(DISPLAY_WIDTH / $scale);
    $cropHeight = (int)round(DISPLAY_HEIGHT / $scale);
    $sourceX = (int)floor(($sourceWidth - $cropWidth) / 2);
    $sourceY = (int)floor(($sourceHeight - $cropHeight) / 2);

    $resampled = imagecopyresampled(
        $output,
        $source,
        0,
        0,
        $sourceX,
        $sourceY,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        $cropWidth,
        $cropHeight
    );
    imagedestroy($source);
    if (!$resampled) {
        imagedestroy($output);
        sendError(500, 'Unable to resize image');
    }

    $jpeg = false;
    foreach ([85, 75, 65, 55, 45] as $quality) {
        ob_start();
        $encoded = imagejpeg($output, null, $quality);
        $candidate = ob_get_clean();
        if (!$encoded || $candidate === false) {
            imagedestroy($output);
            sendError(500, 'Unable to encode JPEG image');
        }
        if (strlen($candidate) <= MAX_OUTPUT_BYTES) {
            $jpeg = $candidate;
            break;
        }
    }
    imagedestroy($output);

    if ($jpeg === false) {
        sendError(413, 'Resized image exceeds the display size limit');
    }

    header('Content-Type: image/jpeg');
    header('Content-Length: ' . strlen($jpeg));
    header('Cache-Control: public, max-age=60');
    header('X-Content-Type-Options: nosniff');
    echo $jpeg;
}

if (($_SERVER['REQUEST_METHOD'] ?? 'GET') !== 'GET') {
    header('Allow: GET');
    sendError(405, 'Method not allowed');
}

if (array_key_exists('image', $_GET)) {
    if (!is_string($_GET['image'])) {
        sendError(400, 'Invalid image parameter');
    }
    serveImage($_GET['image']);
    exit;
}

listImages();