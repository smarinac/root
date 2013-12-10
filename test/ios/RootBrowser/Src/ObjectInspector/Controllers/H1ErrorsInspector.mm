#import <cassert>

#import "ObjectViewController.h"
#import "H1ErrorsInspector.h"

//C++ imports.
#import "TH1.h"

#import "FileUtils.h"

namespace {

const CGFloat defaultCellW = 180.f;
const CGFloat defaultCellH = 44.f;



enum H1ErrorType {
   kNoError,
   kSimple,
   kEdges,
   kRectangles,
   kFill,
   kContour,
   kTotalNumOfTypes
};

NSString *errorTypesStrings[] = {@"No error", @"Simple", @"Edges", @"Rectangles", @"Fill", @"Contour"};

namespace RIB = ROOT::iOS::Browser;
RIB::EHistogramErrorOption histErrorTypes[] = {RIB::hetNoError, RIB::hetE, RIB::hetE1, RIB::hetE2, RIB::hetE3, RIB::hetE4};

}

@implementation H1ErrorsInspector {
   __weak IBOutlet UIPickerView *errorTypePicker;
   __weak ObjectViewController *controller;

   TH1 *object;
}

//____________________________________________________________________________________________________
- (id) initWithNibName : (NSString *)nibNameOrNil bundle : (NSBundle *)nibBundleOrNil
{
   self = [super initWithNibName : nibNameOrNil bundle : nibBundleOrNil];

   if (self) {
      [self view];
   }

   return self;
}

//____________________________________________________________________________________________________
- (void)didReceiveMemoryWarning
{
    // Releases the view if it doesn't have a superview.
    [super didReceiveMemoryWarning];
    
    // Release any cached data, images, etc that aren't in use.
}

#pragma mark - View lifecycle

//____________________________________________________________________________________________________
- (void) viewDidLoad
{
    [super viewDidLoad];
    // Do any additional setup after loading the view from its nib.
}

//____________________________________________________________________________________________________
- (BOOL) shouldAutorotateToInterfaceOrientation : (UIInterfaceOrientation) interfaceOrientation
{
#pragma unused(interfaceOrientation)
	return YES;
}

#pragma mark - Pickerview delegate/data source.

//____________________________________________________________________________________________________
- (CGFloat)pickerView : (UIPickerView *)pickerView widthForComponent : (NSInteger)component
{
   return defaultCellW;
}

//____________________________________________________________________________________________________
- (CGFloat)pickerView : (UIPickerView *)pickerView rowHeightForComponent : (NSInteger)component
{
   return defaultCellH;
}

//____________________________________________________________________________________________________
- (NSInteger)pickerView : (UIPickerView *)pickerView numberOfRowsInComponent : (NSInteger)component
{
   return kTotalNumOfTypes;
}

//____________________________________________________________________________________________________
- (NSInteger)numberOfComponentsInPickerView:(UIPickerView *)pickerView
{
	return 1;
}

//____________________________________________________________________________________________________
- (UIView *)pickerView : (UIPickerView *)pickerView viewForRow : (NSInteger)row forComponent : (NSInteger)component reusingView : (UIView *)view
{
   UILabel *label = [[UILabel alloc] initWithFrame : CGRectMake(0.f, 0.f, defaultCellW, defaultCellH)];
   label.text = errorTypesStrings[row];
   label.font = [UIFont fontWithName : @"TimesNewRomanPS-BoldMT" size : 14.f];
   label.textAlignment = NSTextAlignmentCenter;
   label.backgroundColor = [UIColor colorWithPatternImage : [UIImage imageNamed : @"text_cell_bkn.png"]];

   return label;
}

//____________________________________________________________________________________________________
- (void)pickerView : (UIPickerView *)thePickerView didSelectRow : (NSInteger)row inComponent : (NSInteger)component
{
   if (row >= 0) {
      [controller setErrorOption : histErrorTypes[row]];
      [controller objectWasModifiedUpdateSelection : YES];
   }
}

#pragma mark ObjectInspectorComponent protocol.

//____________________________________________________________________________________________________
- (void) setObjectController : (ObjectViewController *) c
{
   assert(c != nil && "setObjectController:, parameter 'c' is nil");
   controller = c;
}

//____________________________________________________________________________________________________
- (void) setObject : (TObject *) o
{
   assert(o != nullptr && "setObject:, parameter 'o' is null");

   //The type was checked on one level up.
   object = static_cast<TH1*>(o);
   //Read error type from hist.
}

@end