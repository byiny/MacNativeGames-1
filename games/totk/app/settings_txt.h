// The player's settings file, <data>/settings.txt (read by app/launcher.m at each launch; local build only).
// One TOTK_NAME=value per line, '#' starts a comment. A value set here replaces what the app's Info.plist environment gave.
#import <Foundation/Foundation.h>

/* Puts every line of `text` into the environment. Returns 0, or the number of the first bad line (a line that is not
   TOTK_NAME=value with NAME in A-Z, 0-9, _); the lines before it stay set and the lines after it are not read. */
static int settings_txt_apply(NSString *text) {
    NSCharacterSet *ws = NSCharacterSet.whitespaceCharacterSet;
    NSCharacterSet *bad = [NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"].invertedSet;
    __block int n = 0, badline = 0;
    [text enumerateLinesUsingBlock:^(NSString *raw, BOOL *stop) {
        n++;
        NSString *line = [[raw componentsSeparatedByString:@"#"].firstObject stringByTrimmingCharactersInSet:ws];
        if (!line.length) return;
        NSRange eq = [line rangeOfString:@"="];
        NSString *name = eq.location == NSNotFound ? line : [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:ws];
        if (eq.location == NSNotFound || ![name hasPrefix:@"TOTK_"] || [name rangeOfCharacterFromSet:bad].location != NSNotFound) { badline = n; *stop = YES; return; }
        setenv(name.UTF8String, [[line substringFromIndex:eq.location + 1] stringByTrimmingCharactersInSet:ws].UTF8String, 1);
    }];
    return badline;
}
