/* Contributed by Nicola Pero <nicola.pero@meta-innovation.com>, November 2010.  */
/* { dg-do compile } */

#include <objc/objc.h>

@protocol MyProtocol
- (void) message;
@end

@interface MyRootClass
{
  Class isa;
}

/* Test the warnings on 'assign' with protocols.  */
@property id <MyProtocol> property_a;      /* { dg-warning "object property .property.a. has no .assign., .retain. or .copy. attribute" } */
			                   /* { dg-warning ".assign. can be unsafe for Objective-C objects" "" { target *-*-* } 16 } */

@property MyRootClass <MyProtocol> *property_b; /* { dg-warning "object property .property.b. has no .assign., .retain. or .copy. attribute" } */
			                        /* { dg-warning ".assign. can be unsafe for Objective-C objects" "" { target *-*-* } 19 } */

@property Class <MyProtocol> property_c;   /* No 'assign' warning (Classes are static objects so assign semantics do not matter for them). */
@end
